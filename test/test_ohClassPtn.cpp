#include <chrono> //steady_clock, duration
#include <thread> //this_thread
#include <Omega_h_file.hpp>
#include <Omega_h_library.hpp>
#include <Omega_h_array_ops.hpp>
#include <Omega_h_comm.hpp>
#include <Omega_h_mesh.hpp>
#include <Omega_h_for.hpp>
#include <redev_comm.h>
#include "wdmcpl.h"

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

void getClassPtn(Omega_h::Mesh& mesh, redev::LOs& ranks, redev::LOs& classIds) {
  auto ohComm = mesh.comm();
  const auto dim = mesh.dim();
  auto class_ids = mesh.get_array<Omega_h::ClassId>(dim, "class_id");
  Omega_h::ClassId max_class = Omega_h::get_max(class_ids);
  auto max_class_g = ohComm->allreduce(max_class,Omega_h_Op::OMEGA_H_MAX);
  REDEV_ALWAYS_ASSERT(ohComm->size() == max_class_g);
  auto class_ids_h = Omega_h::deep_copy(class_ids);
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

void prepareMsg(Omega_h::Mesh& mesh, redev::ClassPtn& ptn,
    redev::LOs& dest, redev::LOs& offset, redev::LOs& permute) {
  //transfer vtx classification to host
  auto classIds = mesh.get_array<Omega_h::ClassId>(0, "class_id");
  auto classIds_h = Omega_h::deep_copy(classIds);
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
  REDEV_ALWAYS_ASSERT(destRankCounts[0] == 6);
  REDEV_ALWAYS_ASSERT(destRankCounts[1] == 13);
  //create dest and offsets arrays from degree array
  offset.resize(destRankCounts.size()+1);
  dest.resize(destRankCounts.size());
  offset[0] = 0;
  int i = 1;
  for(auto rankCount : destRankCounts) {
    dest[i-1] = rankCount.first;
    offset[i] = offset[i-1]+rankCount.second;
    i++;
  }
  redev::LOs expectedDest = {0,1};
  REDEV_ALWAYS_ASSERT(dest == expectedDest);
  redev::LOs expectedOffset = {0,6,19};
  REDEV_ALWAYS_ASSERT(offset == expectedOffset);
  //fill permutation array such that for vertex i permute[i] contains the
  //  position of vertex i's data in the message array
  std::map<int,int> destRankIdx;
  for(int i=0; i<dest.size(); i++) {
    auto dr = dest[i];
    destRankIdx[dr] = offset[i];
  }
  auto gids = mesh.globals(0);
  auto gids_h = Omega_h::deep_copy(gids);
  permute.resize(classIds_h.size());
  for(auto i=0; i<classIds_h.size(); i++) {
    auto dr = ptn.GetRank(classIds_h[i]);
    auto idx = destRankIdx[dr]++;
    permute[i] = idx;
    printf("i %d gid %ld classId %d idx %d\n", i, gids_h[i], classIds_h[i], permute[i]);
  }
  redev::LOs expectedPermute = {0,6,1,2,3,4,5,7,8,9,10,11,12,13,14,15,16,17,18};
  REDEV_ALWAYS_ASSERT(permute == expectedPermute);
}

int main(int argc, char** argv) {
  auto lib = Omega_h::Library(&argc, &argv);
  auto world = lib.world();
  int rank = world->rank();
  int nproc = world->size();
  if(argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <1=isRendezvousApp,0=isParticipant> /path/to/omega_h/mesh\n";
    std::cerr << "WARNING: this test is currently hardcoded for the xgc1_data/Cyclone_ITG/Cyclone_ITG_deltaf_23mesh mesh\n";
    exit(EXIT_FAILURE);
  }
  OMEGA_H_CHECK(argc == 3);
  auto isRdv = atoi(argv[1]);
  Omega_h::Mesh mesh(&lib);
  Omega_h::binary::read(argv[2], lib.world(), &mesh);
  if(!rank) REDEV_ALWAYS_ASSERT(mesh.nelems() == 23); //sanity check that the loaded mesh is the expected one
  redev::LOs ranks;
  redev::LOs classIds;
  if(isRdv) {
    //partition the omegah mesh by classification and return the
    //rank-to-classid array
    getClassPtn(mesh, ranks, classIds);
    REDEV_ALWAYS_ASSERT(ranks.size()==3);
    REDEV_ALWAYS_ASSERT(ranks.size()==classIds.size());
    Omega_h::vtk::write_parallel("rdvSplit.vtk", &mesh, mesh.dim());
  }
  auto ptn = redev::ClassPtn(ranks,classIds);
  redev::Redev rdv(MPI_COMM_WORLD,ptn,isRdv);
  rdv.Setup();
  const std::string name = "meshVtxIds";
  std::stringstream ss;
  ss << name << " ";
  const int rdvRanks = 2;
  redev::AdiosComm<redev::GO> comm(MPI_COMM_WORLD, rdvRanks, rdv.getToEngine(), rdv.getIO(), name);
  size_t msgStart, msgCount;
  redev::LOs permute;
  redev::LOs dest;
  redev::LOs offsets;
  for(int i=0; i<3; i++) {
    if(!isRdv) {
      //the non-rendezvous app sends mesh data to rendezvous
      //build dest and offsets arrays
      if(i==0) prepareMsg(mesh, ptn, dest, offsets, permute);
      auto gids = mesh.globals(0);
      auto gids_h = Omega_h::deep_copy(gids);
      redev::GOs msgs(gids_h.size());
      for(int i=0; i<msgs.size(); i++)
        msgs[permute[i]] = gids_h[i];
      //fill/access data array - array of vtx global ids
      //pack messages
      auto start = std::chrono::steady_clock::now();
      comm.Pack(dest, offsets, msgs.data());
      comm.Send();
      auto end = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed_seconds = end-start;
      double min, max, avg;
      timeMinMaxAvg(elapsed_seconds.count(), min, max, avg);
      if( i == 0 ) ss << "write";
      std::string str = ss.str();
      if(!rank) printTime(str, min, max, avg);
    } else {
      //the rendezvous app receives mesh data from non-rendezvous
      redev::GO* msgs;
      redev::GOs rdvSrcRanks;
      redev::GOs offsets;
      auto start = std::chrono::steady_clock::now();
      const bool knownSizes = (i == 0) ? false : true;
      comm.Unpack(rdvSrcRanks,offsets,msgs,msgStart,msgCount,knownSizes);
      if(!rank) REDEV_ALWAYS_ASSERT(msgStart==0 && msgCount==6);
      else REDEV_ALWAYS_ASSERT(msgStart==6 && msgCount==13);
      auto end = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed_seconds = end-start;
      double min, max, avg;
      timeMinMaxAvg(elapsed_seconds.count(), min, max, avg);
      if( i == 0 ) ss << "read";
      std::string str = ss.str();
      if(!rank) printTime(str, min, max, avg);
      delete [] msgs;
    }
  }
  return 0;
}
