// topology_fullmesh.cpp
 // All ranks connect to each other, good for small scale (< 8 ranks)
// 

#include <vector>
#include <thread>
#include <cstring>
#include "topology_fullmesh.h"
#include "utils.h"
#include "logger.h"
#include "transport.h"
#include "communicator.h"


// Get the neighbors of rank
std::vector<int> TopologyFullMesh::GetNeighbors(int r) const
{
    std::vector<int> neighbors;
    neighbors.reserve(world_size - 1);
    for (int i = 0; i < world_size; i++)
    {
        // Skip itself
        if (i == r)
        {
            continue;
        }
        neighbors.push_back(i);
    }

    return neighbors;
}

// Two ranks should connect or not
bool TopologyFullMesh::ShouldConnect(int my_rank, int peer_rank) const
{
    // Connect to larger rank only
    return my_rank < peer_rank;
}

// Collective Communication API
bool TopologyFullMesh::Broadcast(Communicator& comm, void* buffer, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;

    if (rank == root)
    {
        // Root rank: Send data to other ranks in threads
        std::vector<std::thread> threads;
        std::vector<bool> results(world_size, true);

        for (int peer = 0; peer < world_size; ++peer)
        {
            if (peer == root)
            {
                // Skip same rank
                continue;
            }

            threads.emplace_back([&comm, peer, buffer, chunk_bytes, &results]()
            {
                auto transport = comm.GetTransport(peer);
                if (!transport)
                {
                    LOG_ERROR("Rank Root: Failed to get transport to peer {} for Broadcast", peer);
                    results[peer] = false;
                    return;
                }

                if (!transport->Send(buffer, chunk_bytes))
                {
                    LOG_ERROR("Rank Root: Failed to broadcast data to peer {}", peer);
                    results[peer] = false;
                    return;
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }

        // Check results
        for (int peer = 0; peer < world_size; ++peer)
        {
            if (peer != root && !results[peer])
            {
                return false;
            }
        }
    }
    else
    {
        // Recv from root rank
        auto transport = comm.GetTransport(root);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to root {} for Broadcast", rank, root);
            return false;
        }

        if (!transport->Recv(buffer, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from root {} for Broadcast", rank, root);
            return false;
        }
    }

    return true;
}

bool TopologyFullMesh::Gather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;

    if (rank == root)
    {
        // Root rank: copy data
        char* recv_ptr = static_cast<char*>(recv_buf);
        memcpy(recv_ptr + root * chunk_bytes, send_buf, chunk_bytes);
        
        // Then recv data from other ranks
        for (int peer = 0; peer < world_size; ++peer)
        {
            if (peer == root)
            {
                // Skip same rank
                continue;
            }

            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank Root: Failed to get transport to rank {} for Gather", rank);
                return false;
            }

            if (!transport->Recv(recv_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank Root: Failed to recv data from peer {} for Gather", peer);
                return false;
            }
        }
    }
    else
    {
        // Send data to root
        auto transport = comm.GetTransport(root);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to root {} for Gather", rank, root);
            return false;
        }

        if (!transport->Send(send_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send data to root {} for Gather", rank, root);
            return false;
        }
    }

    return true;
}

bool TopologyFullMesh::Scatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;

    if (rank == root)
    {
        // Root rank: copy data
        const char* send_ptr = static_cast<const char*>(send_buf);
        memcpy(recv_buf, send_ptr + root * chunk_bytes, chunk_bytes);

        // Then send data to other ranks
        for (int peer = 0; peer < world_size; ++peer)
        {
            if (peer == root)
            {
                // Skip same rank
                continue;
            }

            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank Root: Failed to get transport to rank {} for Scatter", rank);
                return false;
            }

            if (!transport->Send(send_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank Root: Failed to send data to peer {} for Scatter", peer);
                return false;
            }
        }
    }
    else
    {
        // Recv data from root
        auto transport = comm.GetTransport(root);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to root {} for Scatter", rank, root);
            return false;
        }

        if (!transport->Recv(recv_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from root {} for Scatter", rank, root);
            return false;
        }
    }

    return true;
}

bool TopologyFullMesh::Reduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;

    if (rank == root)
    {
        // Root rank: copy data to recv_buf
        memcpy(recv_buf, send_buf, chunk_bytes);

        std::vector<char> temp_buffer;
        temp_buffer.resize(chunk_bytes);
        
        // Recv data from other ranks, then reduce
        for (int peer = 0; peer < world_size; ++peer)
        {
            if (peer == root)
            {
                // Skip same rank
                continue;
            }

            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank Root: Failed to get transport to rank {} for Reduce", rank);
                return false;
            }

            // Recv data to temp_buffer
            if (!transport->Recv(temp_buffer.data(), chunk_bytes))
            {
                LOG_ERROR("Rank Root: Failed to recv data from peer {} for Reduce", peer);
                return false;
            }

            Utils::PerformReduce(temp_buffer.data(), recv_buf, count, dtype, op, world_size);
        }
    }
    else
    {
        // Send data to root
        auto transport = comm.GetTransport(root);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to root {} for Reduce", rank, root);
            return false;
        }

        if (!transport->Send(send_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send data to root {} for Reduce", rank, root);
            return false;
        }
    }

    return true;
}

bool TopologyFullMesh::AllGather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;
    char* recv_ptr = static_cast<char*>(recv_buf);

    // Copy local data
    memcpy(recv_ptr + rank * chunk_bytes, send_buf, chunk_bytes);

    // Use simple all-to-all gather
    for (int peer = 0; peer < world_size; ++peer)
    {
        if (peer == rank)
        {
            // Skip same rank
            continue;
        }

        auto transport = comm.GetTransport(peer);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to peer {} for AllGather", rank, peer);
            return false;
        }

        // Send/Recv for larger peer, Recv/Send for smaller peer
        if (rank < peer)
        {
            if (!transport->Send(send_buf, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to peer {} for AllGather", rank, peer);
                return false;
            }

            if (!transport->Recv(recv_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllGather", rank, peer);
                return false;
            }
        }
        else
        {
            if (!transport->Recv(recv_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllGather", rank, peer);
                return false;
            }

            if (!transport->Send(send_buf, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to peer {} for AllGather", rank, peer);
                return false;
            }
        }
    }

    return true;
}

bool TopologyFullMesh::ReduceScatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;
    size_t total_bytes = chunk_bytes * world_size;

    // Temp buffer to save whole data
    std::vector<char> temp_sendbuf(total_bytes);
    std::vector<char> temp_recvbuf(total_bytes);
    std::vector<char> temp_buf(total_bytes);

    memcpy(temp_recvbuf.data(), send_buf, total_bytes);

    // Recursive Doubling Reduce
    int mask = 1;
    while (mask < world_size)
    {
        int peer = rank ^ mask;
        if (peer < world_size)
        {
            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank {}: Failed to get transport to peer {} for ReduceScatter", rank, peer);
                return false;
            }

            // Send/Recv for larger peer, Recv/Send for smaller peer
            if (rank < peer)
            {
                if (!transport->Send(temp_recvbuf.data(), total_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to send data to peer {} for ReduceScatter", rank, peer);
                    return false;
                }

                if (!transport->Recv(temp_buf.data(), total_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to recv data from peer {} for ReduceScatter", rank, peer);
                    return false;
                }
            }
            else
            {
                if (!transport->Recv(temp_buf.data(), total_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to recv data from peer {} for ReduceScatter", rank, peer);
                    return false;
                }

                if (!transport->Send(temp_recvbuf.data(), total_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to send data to peer {} for ReduceScatter", rank, peer);
                    return false;
                }
            }

            // Reduce
            for (int i = 0; i < world_size; ++i)
            {
                Utils::PerformReduce(temp_buf.data() + i * chunk_bytes, temp_recvbuf.data() + i * chunk_bytes, count, dtype, op, world_size);
            }
        }
        mask <<= 1;
    }

    // Extract my data
    memcpy(recv_buf, temp_recvbuf.data() + rank * chunk_bytes, chunk_bytes);

    return true;
}

bool TopologyFullMesh::AllReduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;

    // Init recv_buf with send_buf
    memcpy(recv_buf, send_buf, chunk_bytes);

    // Prepare temp buffer to recv data before ReduceOp
    std::vector<char> temp_buffer(chunk_bytes);

    // Recursive Doubling
    // mask: 1, 2, 4, 8...
    int mask = 1;
    while (mask < world_size)
    {
        // Calculate the target peer with XOR
        int peer = rank ^ mask;
        if (peer < world_size)
        {
            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank {}: Failed to get transport to peer {} for AllReduce", rank, peer);
                return false;
            }

            // Send/Recv for larger peer, Recv/Send for smaller peer
            if (rank < peer)
            {
                if (!transport->Send(recv_buf, chunk_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to send data to peer {} for AllReduce", rank, peer);
                    return false;
                }

                if (!transport->Recv(temp_buffer.data(), chunk_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduce", rank, peer);
                    return false;
                }
            }
            else
            {
                if (!transport->Recv(temp_buffer.data(), chunk_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduce", rank, peer);
                    return false;
                }

                if (!transport->Send(recv_buf, chunk_bytes))
                {
                    LOG_ERROR("Rank {}: Failed to send data to peer {} for AllReduce", rank, peer);
                    return false;
                }
            }

            // Reduce Operation
            Utils::PerformReduce(temp_buffer.data(), recv_buf, count, dtype, op, world_size);
        }

        mask <<= 1;
    }

    return true;
}

bool TopologyFullMesh::AllToAll(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = type_size * count;
    const char* send_ptr = static_cast<const char*>(send_buf);
    char* recv_ptr = static_cast<char*>(recv_buf);

    // Copy own data
    memcpy(recv_ptr + rank * chunk_bytes, send_ptr + rank * chunk_bytes, chunk_bytes);

    for (int peer = 0; peer < world_size; ++peer)
    {
        if (peer == rank)
        {
            // Skip same rank
            continue;
        }

        auto transport = comm.GetTransport(peer);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to peer {} for AllToAll", rank, peer);
            return false;
        }

        // Send/Recv for larger peer, Recv/Send for smaller peer
        if (rank < peer)
        {
            if (!transport->Send(send_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to peer {} for AllToAll", rank, peer);
                return false;
            }

            if (!transport->Recv(recv_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduce", rank, peer);
                return false;
            }
        }
        else
        {
            if (!transport->Recv(recv_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduce", rank, peer);
                return false;
            }

            if (!transport->Send(send_ptr + peer * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to peer {} for AllToAll", rank, peer);
                return false;
            }
        }
    }

    return true;
}

bool TopologyFullMesh::Barrier(Communicator& comm)
{
    int root = 0;
    int dummy = 0;
    if (rank == root)
    {
        // Recv dummy data from other ranks
        for (int peer = 1; peer < world_size; ++peer)
        {
            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank Root: Failed to get transport to rank {} for Barrier", rank);
                return false;
            }

            // Recv data
            if (!transport->Recv(&dummy, sizeof(int)))
            {
                LOG_ERROR("Rank Root: Failed to recv data from peer {} for Barrier", peer);
                return false;
            }
        }

        // Send ack to other ranks
        for (int peer = 1; peer < world_size; ++peer)
        {
            auto transport = comm.GetTransport(peer);
            if (!transport)
            {
                LOG_ERROR("Rank Root: Failed to get transport to rank {} for Barrier", rank);
                return false;
            }

            // Send data
            if (!transport->Send(&dummy, sizeof(int)))
            {
                LOG_ERROR("Rank Root: Failed to send data to peer {} for Barrier", peer);
                return false;
            }
        }
    }
    else
    {
        // Send dummy data to root
        auto transport = comm.GetTransport(root);
        if (!transport)
        {
            LOG_ERROR("Rank {}: Failed to get transport to root {} for Barrier", rank, root);
            return false;
        }

        if (!transport->Send(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to send data to root {} for Barrier", rank, root);
            return false;
        }

        // Recv ack from root
        if (!transport->Recv(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to recv data from root {} for Barrier", rank, root);
            return false;
        }
    }

    return true;
}
