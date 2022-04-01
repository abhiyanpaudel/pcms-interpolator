#include <chrono> //steady_clock, duration
#include <thread> //this_thread
#include <numeric> // std::iota, std::exclusive_scan
#include <algorithm> // std::sort, std::stable_sort
#include <Omega_h_file.hpp>
#include <Omega_h_library.hpp>
#include <Omega_h_array_ops.hpp>
#include <Omega_h_comm.hpp>
#include <Omega_h_mesh.hpp>
#include <Omega_h_for.hpp>
#include <Omega_h_scalar.hpp> // divide_no_remainder
#include <redev_comm.h>
#include "wdmcpl.h"

struct CSR {
  redev::GOs off;
  redev::GOs val;  
};

struct OutMsg {
  redev::LOs dest;
  redev::LOs offset;
};

struct InMsg {
  redev::GOs srcRanks;
  redev::GOs offset;
  redev::GOs msgs;
  size_t start;
  size_t count;
};

void timeMinMaxAvg(double time, double& min, double& max, double& avg) {
  const auto comm = MPI_COMM_WORLD;
  int nproc;
  MPI_Comm_size(comm, &nproc);
  double tot = 0;
  MPI_Allreduce(&time, &min, 1, MPI_DOUBLE, MPI_MIN, comm);
  MPI_Allreduce(&time, &max, 1, MPI_DOUBLE, MPI_MAX, comm);
  MPI_Allreduce(&time, &tot, 1, MPI_DOUBLE, MPI_SUM, comm);
  avg = tot / nproc;
}

void printTime(std::string mode, double min, double max, double avg) {
  std::cout << mode << " elapsed time min, max, avg (s): "
            << min << " " << max << " " << avg << "\n";
}

template <class T>
void getAndPrintTime(T start, std::string key, int rank) {
  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> elapsed_seconds = end-start;
  double min, max, avg;
  timeMinMaxAvg(elapsed_seconds.count(), min, max, avg);
  if(!rank) printTime(key, min, max, avg);
}

void getClassPtn(Omega_h::Mesh& mesh, redev::LOs& ranks, redev::LOs& classIds) {
  auto ohComm = mesh.comm();
  const auto dim = mesh.dim();
  auto class_ids = mesh.get_array<Omega_h::ClassId>(dim, "class_id");
  Omega_h::ClassId max_class = Omega_h::get_max(class_ids);
  auto max_class_g = ohComm->allreduce(max_class,Omega_h_Op::OMEGA_H_MAX);
  REDEV_ALWAYS_ASSERT(ohComm->size() == max_class_g);
  auto class_ids_h = Omega_h::HostRead(class_ids);
  if(!ohComm->rank()) {
    //send ents with classId=2 to rank 1
    Omega_h::Write<Omega_h::I32> ptnRanks(5,0);
    Omega_h::Write<Omega_h::LO> ptnIdxs(5);
    Omega_h::fill_linear(ptnIdxs,0,1);
    auto owners = Omega_h::Remotes(ptnRanks, ptnIdxs);
    mesh.migrate(owners);
  } else {
    int firstElm = 5;
    const int elms = 18; // migrating elements [5:22]
    Omega_h::Write<Omega_h::I32> ptnRanks(elms,0);
    Omega_h::Write<Omega_h::LO> ptnIdxs(elms);
    Omega_h::fill_linear(ptnIdxs,firstElm,1);
    auto owners = Omega_h::Remotes(ptnRanks, ptnIdxs);
    mesh.migrate(owners);
  }

  //the hardcoded assignment of classids to ranks
  ranks.resize(3);
  classIds.resize(3);
  classIds[0] = 1; ranks[0] = 0;
  classIds[1] = 2; ranks[1] = 1;
  classIds[2] = 3; ranks[2] = 0;  //center ('O point') model vertex
}

void prepareAppOutMessage(Omega_h::Mesh& mesh, const redev::ClassPtn& ptn,
    OutMsg& out, redev::LOs& permute) {
  //transfer vtx classification to host
  auto classIds = mesh.get_array<Omega_h::ClassId>(0, "class_id");
  auto classIds_h = Omega_h::HostRead(classIds);
  //count number of vertices going to each destination process by calling getRank - degree array
  std::map<int,int> destRankCounts;
  const auto ptnRanks = ptn.GetRanks();
  for(auto rank : ptnRanks) {
    destRankCounts[rank] = 0;
  }
  for(auto i=0; i<classIds_h.size(); i++) {
    auto dr = ptn.GetRank(classIds_h[i]);
    assert(destRankCounts.count(dr));
    destRankCounts[dr]++;
  }

  //create dest and offsets arrays from degree array
  out.offset.resize(destRankCounts.size()+1);
  out.dest.resize(destRankCounts.size());
  out.offset[0] = 0;
  int i = 1;
  for(auto rankCount : destRankCounts) {
    out.dest[i-1] = rankCount.first;
    out.offset[i] = out.offset[i-1]+rankCount.second;
    i++;
  }

  //fill permutation array such that for vertex i permute[i] contains the
  //  position of vertex i's data in the message array
  std::map<int,int> destRankIdx;
  for(int i=0; i<out.dest.size(); i++) {
    auto dr = out.dest[i];
    destRankIdx[dr] = out.offset[i];
  }
  auto gids = mesh.globals(0);
  auto gids_h = Omega_h::HostRead(gids);
  permute.resize(classIds_h.size());
  for(auto i=0; i<classIds_h.size(); i++) {
    auto dr = ptn.GetRank(classIds_h[i]);
    auto idx = destRankIdx[dr]++;
    permute[i] = idx;
  }
}

//from https://stackoverflow.com/a/12399290
template <typename T>
std::vector<size_t> sort_indexes(const T &v) {
  // initialize original index locations
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);
  // sort indexes based on comparing values in v
  // using std::stable_sort instead of std::sort
  // to avoid unnecessary index re-orderings
  // when v contains elements of equal values 
  std::stable_sort(idx.begin(), idx.end(),
       [&v](size_t i1, size_t i2) {return v[i1] < v[i2];});
  return idx;
}

//creates the outbound (rdv->non-rdv) permutation CSR given inGids and the rdv mesh instance
//this only needs to be computed once for each topological dimension
void getOutboundRdvPermutation(Omega_h::Mesh& mesh, const redev::GOs& inGids, CSR& perm) {
  auto gids = mesh.globals(0);
  auto gids_h = Omega_h::HostRead(gids);
  auto iGids = sort_indexes(gids_h);
  auto iInGids = sort_indexes(inGids);
  //count the number of times each gid is included in inGids
  perm.off.resize(gids_h.size()+1);
  int j=0;
  for(int i=0; i<inGids.size(); i++) {
    while(gids_h[iGids[j]] != inGids[iInGids[i]] && j < gids_h.size()) {
      j++;
    }
    REDEV_ALWAYS_ASSERT(j!=gids_h.size()); //found
    perm.off[iGids[j]]++;
  }
  //create the offsets array from the counts
  std::exclusive_scan(perm.off.begin(), perm.off.end(), perm.off.begin(), 0); 
  //fill the permutation array
  perm.val.resize(perm.off.back());
  redev::LOs count(gids_h.size()); //how many times each gid was written
  j=0;
  for(int i=0; i<inGids.size(); i++) {
    while(gids_h[iGids[j]] != inGids[iInGids[i]] && j < gids_h.size()) {
      j++;
    }
    REDEV_ALWAYS_ASSERT(j!=gids_h.size()); //found
    const auto subIdx = count[iGids[j]]++;
    const auto startIdx = perm.off[iGids[j]];
    const auto offIdx = startIdx + subIdx;
    perm.val[offIdx] = iInGids[i];
  }
}

void prepareRdvOutMessage(Omega_h::Mesh& mesh, InMsg const& in, OutMsg& out, CSR& permute){
  auto ohComm = mesh.comm();
  const auto rank = ohComm->rank();
  const auto nproc = ohComm->size();
  auto nAppProcs = Omega_h::divide_no_remainder(in.srcRanks.size(),static_cast<size_t>(nproc));
  REDEV_ALWAYS_ASSERT(nAppProcs==2);
  //build dest and offsets arrays from incoming message metadata
  redev::LOs senderDeg(nAppProcs);
  for(int i=0; i<nAppProcs-1; i++) {
    senderDeg[i] = in.srcRanks[(i+1)*nproc+rank] - in.srcRanks[i*nproc+rank];
  }
  const auto totInMsgs = in.offset[rank+1]-in.offset[rank];
  senderDeg[nAppProcs-1] = totInMsgs - in.srcRanks[(nAppProcs-1)*nproc+rank];
  if(!rank) REDEV_ALWAYS_ASSERT( senderDeg == redev::LOs({4,5}) );
  if(rank) REDEV_ALWAYS_ASSERT( senderDeg == redev::LOs({8,7}) );
  for(int i=0; i<nAppProcs; i++) {
    if(senderDeg[i] > 0) {
      out.dest.push_back(i);
    }
  }
  REDEV_ALWAYS_ASSERT( out.dest == redev::LOs({0,1}) );
  redev::GO sum = 0;
  for(auto deg : senderDeg) { //exscan over values > 0
    if(deg>0) {
      out.offset.push_back(sum);
      sum+=deg;
    }
  }
  out.offset.push_back(sum);
  if(!rank) REDEV_ALWAYS_ASSERT( out.offset == redev::LOs({0,4,9}) );
  if(rank) REDEV_ALWAYS_ASSERT( out.offset == redev::LOs({0,8,15}) );
  getOutboundRdvPermutation(mesh, in.msgs, permute);
}



//creates rdvPermute given inGids and the rdv mesh instance
//create rdvPermute such that gids[rdvPermute[i]] == inGids[i]
//for the ith global id in inGids, find the corresponding global id in
// gids (from mesh.globals(0)) and store the index of its position in gids 
// in rdvPermute[i]
//this only needs to be computed once for each topological dimension
//TODO - port to GPU
void getRdvPermutation(Omega_h::Mesh& mesh, const redev::GOs& inGids, redev::GOs& rdvPermute) {
  const auto rank = mesh.comm()->rank();
  auto gids = mesh.globals(0);
  auto gids_h = Omega_h::HostRead(gids);
  auto iGids = sort_indexes(gids_h);
  auto iInGids = sort_indexes(inGids);
  rdvPermute.resize(inGids.size());
  int j=0;
  for(int i=0; i<inGids.size(); i++) {
    while(gids_h[iGids[j]] != inGids[iInGids[i]] && j < gids_h.size()) {
      j++;
    }
    REDEV_ALWAYS_ASSERT(j!=gids_h.size()); //not found
    rdvPermute[iInGids[i]] = iGids[j];
  }
}

void checkAndAttachIds(Omega_h::Mesh& mesh, std::string name, redev::GOs& vtxData, redev::GOs& rdvPermute) {
  REDEV_ALWAYS_ASSERT(rdvPermute.size() == vtxData.size());
  auto gids_h = Omega_h::HostRead(mesh.globals(0));
  const auto numInVtx = rdvPermute.size();
  Omega_h::HostWrite<Omega_h::GO> inVtxData_h(mesh.nverts());
  for(int i=0; i<mesh.nverts(); i++)
    inVtxData_h[i] = -1;
  for(int i=0; i<numInVtx; i++) {
    inVtxData_h[rdvPermute[i]] = vtxData[i];
    REDEV_ALWAYS_ASSERT(gids_h[rdvPermute[i]] == vtxData[i]);
  }
  Omega_h::Write inVtxData(inVtxData_h);
  mesh.add_tag(0,name,1,Omega_h::read(inVtxData));
  mesh.sync_tag(0,name);
}

void writeVtk(Omega_h::Mesh& mesh, std::string name, int step) {
  std::stringstream ss;
  ss << name << step << ".vtk";
  Omega_h::vtk::write_parallel(ss.str(), &mesh, mesh.dim());
}

void unpack(redev::AdiosComm<redev::GO>& comm, bool knownSizes, InMsg& in) {
  redev::GO* msgs;
  comm.Unpack(in.srcRanks, in.offset, msgs, in.start, in.count, knownSizes);
  in.msgs = redev::GOs(msgs, msgs+in.count);
  delete [] msgs;
}

int main(int argc, char** argv) {
  auto lib = Omega_h::Library(&argc, &argv);
  auto world = lib.world();
  const int rank = world->rank();
  const int nproc = world->size();
  if(argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <1=isRendezvousApp,0=isParticipant> /path/to/omega_h/mesh\n";
    std::cerr << "WARNING: this test is currently hardcoded for the xgc1_data/Cyclone_ITG/Cyclone_ITG_deltaf_23mesh/mesh.osh\n";
    std::cerr << "mesh for the rendezvous processes and xgc1_data/Cyclone_ITG/Cyclone_ITG_deltaf_23mesh mesh/2p.osh for the\n";
    std::cerr << "for the non-rendezvous processes\n";
    exit(EXIT_FAILURE);
  }
  OMEGA_H_CHECK(argc == 3);
  auto isRdv = atoi(argv[1]);
  Omega_h::Mesh mesh(&lib);
  Omega_h::binary::read(argv[2], lib.world(), &mesh);
  redev::LOs ranks;
  redev::LOs classIds;
  if(isRdv) {
    //partition the omegah mesh by classification and return the
    //rank-to-classid array
    getClassPtn(mesh, ranks, classIds);
    REDEV_ALWAYS_ASSERT(ranks.size()==3);
    REDEV_ALWAYS_ASSERT(ranks.size()==classIds.size());
    Omega_h::vtk::write_parallel("rdvSplit.vtk", &mesh, mesh.dim());
  } else {
    REDEV_ALWAYS_ASSERT(world->size()==2);
    if(!rank) REDEV_ALWAYS_ASSERT(mesh.nelems()==11);
    Omega_h::vtk::write_parallel("appSplit.vtk", &mesh, mesh.dim());
  }
  auto ptn = redev::ClassPtn(ranks,classIds);
  redev::Redev rdv(MPI_COMM_WORLD,ptn,isRdv);
  rdv.Setup();

  const std::string name = "meshVtxIds";
  const int rdvRanks = 2;
  const int appRanks = 2;
  redev::AdiosComm<redev::GO> commA2R(MPI_COMM_WORLD, rdvRanks, rdv.getToEngine(), rdv.getToIO(), name+"_A2R");
  redev::AdiosComm<redev::GO> commR2A(MPI_COMM_WORLD, appRanks, rdv.getFromEngine(), rdv.getFromIO(), name+"_R2A");

  redev::LOs appOutPermute;
  OutMsg appOut;
  InMsg appIn;

  redev::GOs rdvInPermute;
  CSR rdvOutPermute;
  OutMsg rdvOut;
  InMsg rdvIn;

  for(int iter=0; iter<3; iter++) {
    if(!rank) fprintf(stderr, "isRdv %d iter %d\n", isRdv, iter);
    MPI_Barrier(MPI_COMM_WORLD);
    //////////////////////////////////////////////////////
    //the non-rendezvous app sends global vtx ids to rendezvous
    //////////////////////////////////////////////////////
    if(!isRdv) {
      //build dest, offsets, and permutation arrays
      if(iter==0) prepareAppOutMessage(mesh, ptn, appOut, appOutPermute);
      //fill message array
      auto gids = mesh.globals(0);
      auto gids_h = Omega_h::HostRead(gids);
      redev::GOs msgs(gids_h.size(),0);
      for(int i=0; i<msgs.size(); i++) {
        msgs[appOutPermute[i]] = gids_h[i];
      }
      auto start = std::chrono::steady_clock::now();
      commA2R.Pack(appOut.dest, appOut.offset, msgs.data());
      commA2R.Send();
      getAndPrintTime(start,name + " appWrite",rank);
    } else {
      auto start = std::chrono::steady_clock::now();
      const bool knownSizes = (iter == 0) ? false : true;
      unpack(commA2R,knownSizes,rdvIn);
      getAndPrintTime(start,name + " rdvRead",rank);
      //attach the ids to the mesh
      if(iter==0) getRdvPermutation(mesh, rdvIn.msgs, rdvInPermute);
      checkAndAttachIds(mesh, "inVtxGids", rdvIn.msgs, rdvInPermute);
      writeVtk(mesh,"rdvInGids",iter);
    } //end non-rdv -> rdv
    //////////////////////////////////////////////////////
    //the rendezvous app sends global vtx ids to non-rendezvous
    //////////////////////////////////////////////////////
    if(isRdv) {
      //build dest, offsets, and permutation arrays
      if( iter==0 ) prepareRdvOutMessage(mesh,rdvIn,rdvOut,rdvOutPermute);
      //fill message array
      auto gids = mesh.globals(0);
      auto gids_h = Omega_h::HostRead(gids);
      redev::GOs msgs(rdvOutPermute.off.back());
      for(int i=0; i<gids_h.size(); i++) {
        for(int j=rdvOutPermute.off[i]; j<rdvOutPermute.off[i+1]; j++) {
          msgs[rdvOutPermute.val[j]] = gids_h[i];
        }
      }
      auto start = std::chrono::steady_clock::now();
      commR2A.Pack(rdvOut.dest, rdvOut.offset, msgs.data());
      commR2A.Send();
      getAndPrintTime(start,name + " rdvWrite",rank);
    } else {
      auto start = std::chrono::steady_clock::now();
      const bool knownSizes = (iter == 0) ? false : true;
      unpack(commR2A,knownSizes,appIn);
      getAndPrintTime(start,name + " appRead",rank);
      { //check incoming messages are in the correct order
        auto gids = mesh.globals(0);
        auto gids_h = Omega_h::HostRead(gids);
        REDEV_ALWAYS_ASSERT(appIn.count == gids_h.size());
        for(int i=0; i<appIn.msgs.size(); i++) {
          REDEV_ALWAYS_ASSERT(gids_h[i] == appIn.msgs[appOutPermute[i]]);
        }
      }
    } //end rdv -> non-rdv
  } //end iter loop
  return 0;
}
