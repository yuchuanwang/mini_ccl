/*
Usage: 
  Manual mode:  test_ccl <rank> <world_size> [transport] [topology]
  MPI mode:     mpirun -np 4 test_ccl [transport] [topology]

  transport: tcp (default) | rdma
  topology:  ring (default) | fullmesh

Examples:
  # Manual mode (multiple terminals)
  ./test_ccl 0 4                    # TCP + Ring (default)
  ./test_ccl 0 4 rdma               # RDMA + Ring
  ./test_ccl 0 4 tcp fullmesh       # TCP + FullMesh

  # MPI mode (single command)
  mpirun -np 4 ./test_ccl           # TCP + Ring (default)
  mpirun -np 4 ./test_ccl rdma      # RDMA + Ring
  mpirun -np 4 ./test_ccl tcp fullmesh  # TCP + FullMesh
*/
#include <iostream>
#include <vector>
#include <cstdlib>
#include <cstring>
#include "types.h"
#include "logger.h"
#include "communicator.h"

void PrintUsage(const char* prog)
{
    std::cout << "Usage:" << std::endl;
    std::cout << "  Manual: " << prog << " <rank> <world_size> [transport] [topology]" << std::endl;
    std::cout << "  MPI:    mpirun -np <N> " << prog << " [transport] [topology]" << std::endl;
    std::cout << std::endl;
    std::cout << "  transport: tcp (default) | rdma" << std::endl;
    std::cout << "  topology:  ring (default) | fullmesh" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog << " 0 4 rdma ring      # Manual mode" << std::endl;
    std::cout << "  mpirun -np 4 " << prog << " rdma  # MPI mode" << std::endl;
}

// Try to get rank/world_size from MPI environment variables
bool GetMPIEnv(int& rank, int& world_size)
{
    // OpenMPI
    const char* ompi_rank = std::getenv("OMPI_COMM_WORLD_RANK");
    const char* ompi_size = std::getenv("OMPI_COMM_WORLD_SIZE");
    if (ompi_rank && ompi_size)
    {
        rank = std::atoi(ompi_rank);
        world_size = std::atoi(ompi_size);
        return true;
    }

    // MPICH / Intel MPI
    const char* pmi_rank = std::getenv("PMI_RANK");
    const char* pmi_size = std::getenv("PMI_SIZE");
    if (pmi_rank && pmi_size)
    {
        rank = std::atoi(pmi_rank);
        world_size = std::atoi(pmi_size);
        return true;
    }

    // Slurm
    const char* slurm_rank = std::getenv("SLURM_PROCID");
    const char* slurm_size = std::getenv("SLURM_NTASKS");
    if (slurm_rank && slurm_size)
    {
        rank = std::atoi(slurm_rank);
        world_size = std::atoi(slurm_size);
        return true;
    }

    return false;
}

void TestBroadcast(Communicator& comm)
{
    int rank = comm.GetRank();
    //int world_size = comm.GetWorldSize();
    
    // Prepare data
    size_t count = 10;
    std::vector<float> data(count);
    if (rank == 0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            data[i] = static_cast<float>(i);
        }
    }

    // Broadcast data from rank 0 to other ranks
    if (!comm.Broadcast(data.data(), data.size(), DataType::FLOAT32, 0))
    {
        LOG_ERROR("Rank {}: Failed to Broadcast", rank);
        return;
    }

    // Verify
    LOG_INFO("Rank {}: Broadcast results:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << data[i] << " ";
    }
    std::cout << std::endl;
}

void TestGather(Communicator& comm)
{
    int rank = comm.GetRank();
    int world_size = comm.GetWorldSize();
    
    // Prepare data to be gathered
    size_t count = 10;
    size_t total_count = count * world_size;
    std::vector<float> send_buf(count);
    for (size_t i = 0; i < count; ++i)
    {
        send_buf[i] = static_cast<float>(rank * 10 + i);
    }

    LOG_INFO("Rank {}: Data ready for Gather:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << send_buf[i] << " ";
    }
    std::cout << std::endl;


    std::vector<float> recv_buf;
    if (rank == 0)
    {
        // Prepare buffer on rank 0 to gather data from other ranks
        recv_buf.resize(total_count);
    }

    if (!comm.Gather(send_buf.data(), recv_buf.data(), send_buf.size(), DataType::FLOAT32, 0))
    {
        LOG_ERROR("Rank {}: Failed to Gather", rank);
        return;
    }

    // Print data on rank 0
    if (rank == 0)
    {
        LOG_INFO("Rank {}: Gather results:", rank);
        for (size_t i = 0; i < total_count; ++i)
        {
            std::cout << recv_buf[i] << " ";
        }
        std::cout << std::endl;
    }
}

void TestScatter(Communicator& comm)
{
    int rank = comm.GetRank();
    int world_size = comm.GetWorldSize();
    
    // Prepare data to be scattered
    size_t count = 10;
    size_t total_count = count * world_size;
    std::vector<float> send_buf(count);
    if (rank == 0)
    {
        send_buf.resize(total_count);
        for (size_t i = 0; i < total_count; ++i)
        {
            send_buf[i] = static_cast<float>(i);
        }

        LOG_INFO("Rank {}: Data ready for Scatter:", rank);
        for (size_t i = 0; i < total_count; ++i)
        {
            std::cout << send_buf[i] << " ";
        }
        std::cout << std::endl;
    }

    std::vector<float> recv_buf(count);
    if (!comm.Scatter(send_buf.data(), recv_buf.data(), recv_buf.size(), DataType::FLOAT32, 0))
    {
        LOG_ERROR("Rank {}: Failed to Scatter", rank);
        return;
    }

    LOG_INFO("Rank {}: Scatter results:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << recv_buf[i] << " ";
    }
    std::cout << std::endl;
}

void TestReduce(Communicator& comm)
{
    int rank = comm.GetRank();
    //int world_size = comm.GetWorldSize();
    
    // Prepare data
    size_t count = 10;
    std::vector<float> send_buf(count);
    for (size_t i = 0; i < count; ++i)
    {
        send_buf[i] = static_cast<float>(rank + 1);
    }
    LOG_INFO("Rank {}: Original data:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << send_buf[i] << " ";
    }
    std::cout << std::endl;

    std::vector<float> recv_buf(count);

    // Run Reduce SUM on rank 0
    if (!comm.Reduce(send_buf.data(), recv_buf.data(), send_buf.size(), DataType::FLOAT32, ReduceOp::SUM, 0))
    {
        LOG_ERROR("Rank {}: Failed to Reduce", rank);
        return;
    }

    // Verify
    if (rank == 0)
    {
        LOG_INFO("Rank {}: Reduce results:", rank);
        for (size_t i = 0; i < count; ++i)
        {
            std::cout << recv_buf[i] << " ";
        }
        std::cout << std::endl;
    }
}

void TestAllReduce(Communicator& comm)
{
    int rank = comm.GetRank();
    //int world_size = comm.GetWorldSize();
    
    // Prepare data
    size_t count = 10;
    std::vector<float> data(count);
    for (size_t i = 0; i < count; ++i)
    {
        data[i] = static_cast<float>(rank + 1);
    }
    LOG_INFO("Rank {}: Original data:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << data[i] << " ";
    }
    std::cout << std::endl;

    // Run AllReduce SUM
    if (!comm.AllReduce(data.data(), data.data(), count, DataType::FLOAT32, ReduceOp::SUM))
    {
        LOG_ERROR("Rank {}: Failed to AllReduce", rank);
        return;
    }

    // Verify
    LOG_INFO("Rank {}: AllReduce results:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << data[i] << " ";
    }
    std::cout << std::endl;
}

void TestAllGather(Communicator& comm)
{
    int rank = comm.GetRank();
    int world_size = comm.GetWorldSize();
    
    // Prepare data to be gathered
    size_t count = 10;
    size_t total_count = count * world_size;
    std::vector<float> send_buf(count);
    for (size_t i = 0; i < count; ++i)
    {
        send_buf[i] = static_cast<float>(rank * 10 + i);
    }

    LOG_INFO("Rank {}: Data ready for AllGather:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << send_buf[i] << " ";
    }
    std::cout << std::endl;


    std::vector<float> recv_buf;
    // Prepare buffer to gather data from other ranks
    recv_buf.resize(total_count);

    if (!comm.AllGather(send_buf.data(), recv_buf.data(), send_buf.size(), DataType::FLOAT32))
    {
        LOG_ERROR("Rank {}: Failed to AllGather", rank);
        return;
    }

    // Print data
    LOG_INFO("Rank {}: AllGather results:", rank);
    for (size_t i = 0; i < total_count; ++i)
    {
        std::cout << recv_buf[i] << " ";
    }
    std::cout << std::endl;
}

void TestReduceScatter(Communicator& comm)
{
    int rank = comm.GetRank();
    int world_size = comm.GetWorldSize();

    // Prepare data
    size_t count = 10;
    size_t total_count = count * world_size;
    std::vector<float> send_buf(total_count);
    for (size_t i = 0; i < total_count; ++i)
    {
        send_buf[i] = static_cast<float>(rank + 1);
    }

    LOG_INFO("Rank {}: Data ready for ReduceScatter:", rank);
    for (size_t i = 0; i < total_count; ++i)
    {
        std::cout << send_buf[i] << " ";
    }
    std::cout << std::endl;

    // Prepare buffer to recv data from other ranks
    std::vector<float> recv_buf(count);
    if (!comm.ReduceScatter(send_buf.data(), recv_buf.data(), recv_buf.size(), DataType::FLOAT32, ReduceOp::SUM))
    {
        LOG_ERROR("Rank {}: Failed to ReduceScatter", rank);
        return;
    }

    // Print data
    LOG_INFO("Rank {}: ReduceScatter results:", rank);
    for (size_t i = 0; i < count; ++i)
    {
        std::cout << recv_buf[i] << " ";
    }
    std::cout << std::endl;
}

void TestAllToAll(Communicator& comm)
{
    int rank = comm.GetRank();
    int world_size = comm.GetWorldSize();

    // Prepare data
    size_t count = 10;
    size_t total_count = count * world_size;
    std::vector<float> send_buf(total_count);
    for (int r = 0; r < world_size; ++r)
    {
        for (size_t i = 0; i < count; ++i)
        {
            send_buf[r * count + i] = static_cast<float>(rank * 100 + r * count + i);
        }
    }

    LOG_INFO("Rank {}: Data ready for AllToAll:", rank);
    for (size_t i = 0; i < total_count; ++i)
    {
        std::cout << send_buf[i] << " ";
    }
    std::cout << std::endl;

    // Prepare buffer to recv data from other ranks
    std::vector<float> recv_buf(total_count);
    if (!comm.AllToAll(send_buf.data(), recv_buf.data(), count, DataType::FLOAT32))
    {
        LOG_ERROR("Rank {}: Failed to AllToAll", rank);
        return;
    }

    // Print data
    LOG_INFO("Rank {}: AllToAll results:", rank);
    for (size_t i = 0; i < total_count; ++i)
    {
        std::cout << recv_buf[i] << " ";
    }
    std::cout << std::endl;
}

// Helper to parse transport string
TransportType ParseTransport(const char* str)
{
    if (std::strcmp(str, "rdma") == 0 || std::strcmp(str, "RDMA") == 0)
        return TransportType::RDMA;
    return TransportType::TCP;
}

// Helper to parse topology string
TopologyType ParseTopology(const char* str)
{
    if (std::strcmp(str, "fullmesh") == 0 || std::strcmp(str, "FULLMESH") == 0 || 
        std::strcmp(str, "full_mesh") == 0 || std::strcmp(str, "mesh") == 0)
        return TopologyType::FULL_MESH;
    return TopologyType::RING;
}

int main(int argc, char** argv)
{
    Logger::Instance().SetLogLevel(LogLevel::INFO);

    int rank = -1;
    int world_size = -1;
    TransportType transport = TransportType::TCP;
    TopologyType topology = TopologyType::RING;

    // Try MPI environment first
    bool mpi_mode = GetMPIEnv(rank, world_size);

    if (mpi_mode)
    {
        // MPI mode: mpirun -np 4 ./test_ccl [transport] [topology]
        if (argc >= 2)
            transport = ParseTransport(argv[1]);
        if (argc >= 3)
            topology = ParseTopology(argv[2]);
    }
    else
    {
        // Manual mode: ./test_ccl <rank> <world_size> [transport] [topology]
        if (argc < 3)
        {
            PrintUsage(argv[0]);
            return 1;
        }
        rank = std::atoi(argv[1]);
        world_size = std::atoi(argv[2]);
        if (argc >= 4)
            transport = ParseTransport(argv[3]);
        if (argc >= 5)
            topology = ParseTopology(argv[4]);
    }

    // Print configuration
    const char* transport_str = (transport == TransportType::TCP) ? "TCP" : "RDMA";
    const char* topology_str = (topology == TopologyType::RING) ? "Ring" : "FullMesh";
    LOG_INFO("Configuration: rank={}, world_size={}, transport={}, topology={}", 
             rank, world_size, transport_str, topology_str);

    // Config
    CommConfig config;
    config.rank = rank;
    config.world_size = world_size;
    config.master_addr = "127.0.0.1";
    config.master_port = 12321;
    
    // Init communicator
    Communicator comm;
    if (!comm.Init(config, transport, topology))
    {
        LOG_ERROR("Rank {}: Failed to init", rank);
        return 1;
    }
    LOG_INFO("Rank {}: Init", rank);
    std::cout << std::endl;

    LOG_INFO("Rank {}: Test Broadcast", rank);
    TestBroadcast(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test Scatter", rank);
    TestScatter(comm);
    std::cout << std::endl;
    comm.Barrier();
    LOG_INFO("Rank {}: Test Gather", rank);
    TestGather(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test AllGather", rank);
    TestAllGather(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test ReduceScatter", rank);
    TestReduceScatter(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test Reduce", rank);
    TestReduce(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test AllReduce", rank);
    TestAllReduce(comm);
    std::cout << std::endl;
    comm.Barrier();

    LOG_INFO("Rank {}: Test AllToAll", rank);
    TestAllToAll(comm);
    std::cout << std::endl;
    comm.Barrier();

    return 0;
}
