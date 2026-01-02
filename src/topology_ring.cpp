// topology_ring.cpp
// All ranks connect as a ring, good for middle scale
// 

#include <vector>
#include <algorithm>
#include <thread>
#include <cstring>
#include "topology_ring.h"
#include "logger.h"
#include "utils.h"
#include "transport.h"
#include "communicator.h"

// Get the neighbors of rank
std::vector<int> TopologyRing::GetNeighbors(int r) const
{
    std::vector<int> neighbors;
    int prev = GetPrevRank(r);
    int next = GetNextRank(r);

    neighbors.push_back(prev);
    if (next != prev)
    {
        neighbors.push_back(next);
    }

    return neighbors;
}

// Two ranks should connect or not
bool TopologyRing::ShouldConnect(int my_rank, int peer_rank) const
{
    int prev = GetPrevRank(my_rank);
    int next = GetNextRank(my_rank);

    if (peer_rank == next && my_rank < peer_rank)
    {
        return true;
    }
    if (peer_rank == prev && my_rank < peer_rank)
    {
        return true;
    }

    return false;
}

// Ring specific
int TopologyRing::GetPrevRank(int r) const
{
    return (r - 1 + world_size) % world_size;
}

int TopologyRing::GetNextRank(int r) const
{
    return (r + 1) % world_size;
}

bool TopologyRing::GetRingTransports(Communicator& comm, RingTransports& rt, int world_size)
{
    rt.prev_rank = (rank - 1 + world_size) % world_size;
    rt.next_rank = (rank + 1) % world_size;
    rt.prev = comm.GetTransport(rt.prev_rank);
    rt.next = comm.GetTransport(rt.next_rank);

    if (!rt.prev || !rt.next)
    {
        LOG_ERROR("Rank {}: Failed to get ring transports", rank);
        return false;
    }
    return true;
}

// Collective Communication API
bool TopologyRing::Broadcast(Communicator& comm, void* buffer, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for Broadcast", rank);
        return false;
    }

    if (rank == root)
    {
        // Send to next rank
        if (rt.next_rank != root)
        {
            if (!rt.next->Send(buffer, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send to next rank for Broadcast", rank);
                return false;
            }
        }
    }
    else
    {
        // Recv from previous rank
        if (!rt.prev->Recv(buffer, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv from previuos rank for Broadcast", rank);
            return false;
        }

        // Send to next rank (if next is not root)
        if (rt.next_rank != root)
        {
            if (!rt.next->Send(buffer, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send to next rank for Broadcast", rank);
                return false;
            }
        }
    }

    return true;
}

// Ring Gather: Data flows along the ring towards root (reverse of Scatter)
// Each node concatenates received data with its own data and sends to prev
// For example, if we have 4 ranks, and root is 0:
// rank 3: send [data[3]] (1 chunk)
// rank 2: recv [data[3]], send [data[2], data[3]] (2 chunks)
// rank 1: recv [data[2], data[3]], send [data[1], data[2], data[3]] (3 chunks)
// rank 0: recv [data[1], data[2], data[3]], copy own data[0]
bool TopologyRing::Gather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for Gather", rank);
        return false;
    }

    // Calculate position in the gather chain
    // position 0 = root, position world_size-1 = farthest node from root
    int position = (rank - root + world_size) % world_size;

    if (rank == root)
    {
        char* recv_ptr = static_cast<char*>(recv_buf);

        // Root: copy own data first
        memcpy(recv_ptr + root * chunk_bytes, send_buf, chunk_bytes);

        // Receive all other data in one batch (world_size - 1 chunks)
        // Data arrives as: [data[root+1], data[root+2], ..., data[root-1]]
        size_t recv_bytes = (world_size - 1) * chunk_bytes;
        std::vector<char> temp(recv_bytes);
        if (!rt.next->Recv(temp.data(), recv_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv gathered data", rank);
            return false;
        }

        // Copy received data to correct positions
        for (int i = 0; i < world_size - 1; ++i)
        {
            int source_rank = (root + 1 + i) % world_size;
            memcpy(recv_ptr + source_rank * chunk_bytes, temp.data() + i * chunk_bytes, chunk_bytes);
        }
    }
    else
    {
        // Number of chunks I need to send (my own + all nodes farther than me)
        int chunks_to_send = world_size - position;

        // Allocate buffer for concatenated data
        std::vector<char> send_buffer(chunks_to_send * chunk_bytes);

        // Copy my own data first
        memcpy(send_buffer.data(), send_buf, chunk_bytes);

        // If not the farthest node, receive data from next and append
        if (position != world_size - 1)
        {
            size_t recv_bytes = (chunks_to_send - 1) * chunk_bytes;
            if (!rt.next->Recv(send_buffer.data() + chunk_bytes, recv_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data for Gather", rank);
                return false;
            }
        }

        // Send concatenated data towards root
        if (!rt.prev->Send(send_buffer.data(), chunks_to_send * chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send data for Gather", rank);
            return false;
        }
    }

    return true;
}

bool TopologyRing::Scatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for Scatter", rank);
        return false;
    }

    if (rank == root)
    {
        // Copy the data for itself, and then send pieces of data to other ranks
        const char* send_ptr = static_cast<const char*>(send_buf);
        // Calculate the offset to copy
        memcpy(recv_buf, send_ptr + root * chunk_bytes, chunk_bytes);

        // Send data to other rank with the ring
        for (int step = 1; step < world_size; ++ step)
        {
            int target_rank = (root + step) % world_size;
            if (!rt.next->Send(send_ptr + target_rank * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to rank {} for Scatter", rank, target_rank);
                return false;
            }
        }
    }
    else
    {
        // For non-root, recv data
        if (!rt.prev->Recv(recv_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from previous for Scatter", rank);
            return false;
        }

        // Forward remaining data
        int my_pos = (rank - root + world_size) % world_size;
        // How many times of forwarding needed
        int steps_to_forward = world_size - 1 - my_pos;
        for (int step = 0; step < steps_to_forward; ++step)
        {
            std::vector<char> temp(chunk_bytes);
            if (!rt.prev->Recv(temp.data(), chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv forwarded data from previous for Scatter", rank);
                return false;
            }
            if (!rt.next->Send(temp.data(), chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send forwarded data to next for Scatter", rank);
                return false;
            }
        }
    }

    return true;
}

// Ring Reduce: Data flows along the ring towards root
// Each node receives from next, reduces with local data, sends to prev
// For example, if we have 4 ranks, and root is 0, the data flows like this:
// 0 -> 1 -> 2 -> 3 -> 0
// Data flow: 
// rank 3 send -> 
// rank 2 reduce + send -> 
// rank 1 reduce + send -> 
// rank 0 (root) reduce + end
bool TopologyRing::Reduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for Reduce", rank);
        return false;
    }

    // Calculate position in the reduce chain
    // position 0 = root, position world_size-1 = farthest node from root
    int position = (rank - root + world_size) % world_size;

    std::vector<char> temp_buffer(chunk_bytes);

    if (rank == root)
    {
        // Root: copy local data to recv_buf, then receive and reduce
        memcpy(recv_buf, send_buf, chunk_bytes);

        if (!rt.next->Recv(temp_buffer.data(), chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv for Reduce", rank);
            return false;
        }

        Utils::PerformReduce(temp_buffer.data(), recv_buf, count, dtype, op, world_size);
    }
    else
    {
        // Non-root nodes: copy local data to temp buffer
        memcpy(temp_buffer.data(), send_buf, chunk_bytes);

        // If not the farthest node, receive from next and reduce
        if (position != world_size - 1)
        {
            std::vector<char> recv_temp(chunk_bytes);
            if (!rt.next->Recv(recv_temp.data(), chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv for Reduce", rank);
                return false;
            }
            Utils::PerformReduce(recv_temp.data(), temp_buffer.data(), count, dtype, op, world_size);
        }

        // Send towards root (via prev)
        if (!rt.prev->Send(temp_buffer.data(), chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send for Reduce", rank);
            return false;
        }
    }

    return true;
}

bool TopologyRing::AllGather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;
    char* recv_ptr = static_cast<char*>(recv_buf);

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for AllGather", rank);
        return false;
    }

    // Copy data of itself
    memcpy(recv_ptr + rank * chunk_bytes, send_buf, chunk_bytes);

    // Ring AllGather
    // Each step, each rank forwards the data recv to next rank
    for (int step = 0; step < world_size - 1; ++step)
    {
        int send_rank = (rank - step + world_size) % world_size;
        int recv_rank = (rank - step + world_size - 1) % world_size;

        // Send first for even; Recv first for odd
        if (rank % 2 == 0)
        {
            if (!rt.next->Send(recv_ptr + send_rank * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to next for AllGather in step {}", rank, step);
                return false;
            }
            if (!rt.prev->Recv(recv_ptr + recv_rank * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from previous for AllGather in step {}", rank, step);
                return false;
            }
        }
        else
        {
            if (!rt.prev->Recv(recv_ptr + recv_rank * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to recv data from previous for AllGather in step {}", rank, step);
                return false;
            }
            if (!rt.next->Send(recv_ptr + send_rank * chunk_bytes, chunk_bytes))
            {
                LOG_ERROR("Rank {}: Failed to send data to next for AllGather in step {}", rank, step);
                return false;
            }
        }
    }

    return true;
}

// ReduceScatter = Reduce + Scatter
// 1. Reduce all data to root (rank 0)
// 2. Scatter the reduced result to all ranks
bool TopologyRing::ReduceScatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;
    size_t total_bytes = chunk_bytes * world_size;
    int root = 0;

    // Buffer for reduced result (only meaningful on root)
    std::vector<char> reduced_buf(total_bytes);

    // Step 1: Reduce all data to root
    if (!Reduce(comm, send_buf, reduced_buf.data(), count * world_size, dtype, op, root))
    {
        LOG_ERROR("Rank {}: Failed to Reduce for ReduceScatter", rank);
        return false;
    }

    // Step 2: Scatter the reduced result
    if (!Scatter(comm, reduced_buf.data(), recv_buf, count, dtype, root))
    {
        LOG_ERROR("Rank {}: Failed to Scatter for ReduceScatter", rank);
        return false;
    }

    return true;
}

// Special case for 2 ranks
bool TopologyRing::AllReduceTwoRanks(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;
    int peer = 1 - rank;

    auto transport = comm.GetTransport(peer);
    if (!transport)
    {
        LOG_ERROR("Rank {}: Failed to get transport to peer {} for AllReduceTwoRanks", rank, peer);
        return false;
    }

    // Copy data from send_buf to recv_buf
    memcpy(recv_buf, send_buf, chunk_bytes);

    // Buffer for peer data
    std::vector<char> peer_data(chunk_bytes);

    // Swap data
    if (rank == 0)
    {
        if (!transport->Send(recv_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send data to peer {} for AllReduceTwoRanks", rank, peer);
            return false;
        }
        if (!transport->Recv(peer_data.data(), chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduceTwoRanks", rank, peer);
            return false;
        }
    }
    else
    {
        if (!transport->Recv(peer_data.data(), chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from peer {} for AllReduceTwoRanks", rank, peer);
            return false;
        }
        if (!transport->Send(recv_buf, chunk_bytes))
        {
            LOG_ERROR("Rank {}: Failed to send data to peer {} for AllReduceTwoRanks", rank, peer);
            return false;
        }
    }

    // Reduce data
    Utils::PerformReduce(peer_data.data(), recv_buf, count, dtype, op, 2);
    return true;
}

bool TopologyRing::AllReduceReduceScatter(void* recv_buf, size_t count, DataType dtype, ReduceOp op, RingTransports& rt)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_size = (count + world_size - 1) / world_size;
    size_t chunk_bytes = chunk_size * type_size;

    std::vector<char> temp_buffer(chunk_bytes);
    char* data = static_cast<char*>(recv_buf);

    for (int step = 0; step < world_size - 1; ++step)
    {
        int send_chunk = (rank - step + world_size) % world_size;
        int recv_chunk = (rank - step + world_size - 1) % world_size;

        size_t send_offset = send_chunk * chunk_bytes;
        size_t recv_offset = recv_chunk * chunk_bytes;

        size_t actual_send_count = std::min(chunk_size, count - send_chunk * chunk_size);
        size_t actual_recv_count = std::min(chunk_size, count - recv_chunk * chunk_size);
        size_t actual_send_bytes = actual_send_count * type_size;
        size_t actual_recv_bytes = actual_recv_count * type_size;

        // Send data in a new thread
        std::thread send_thread([&rt, data, send_offset, actual_send_bytes]()
        {
            rt.next->Send(data + send_offset, actual_send_bytes);
        });

        // Recv data in current thread
        if (!rt.prev->Recv(temp_buffer.data(), actual_recv_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from prev for AllReduceReduceScatter in step {}", rank, step);
            send_thread.join();
            return false;
        }

        send_thread.join();

        Utils::PerformReduce(temp_buffer.data(), data + recv_offset, actual_recv_count, dtype, op, world_size);
    }

    return true;
}

bool TopologyRing::AllReduceAllGather(void* recv_buf, size_t count, DataType dtype, RingTransports& rt)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_size = (count + world_size - 1) / world_size;
    size_t chunk_bytes = chunk_size * type_size;
    char* data = static_cast<char*>(recv_buf);
    
    for (int step = 0; step < world_size - 1; ++step)
    {
        int send_chunk = (rank - step + 1 + world_size) % world_size;
        int recv_chunk = (rank - step + world_size) % world_size;
        
        size_t send_offset = send_chunk * chunk_bytes;
        size_t recv_offset = recv_chunk * chunk_bytes;
        
        size_t actual_send_count = std::min(chunk_size, count - send_chunk * chunk_size);
        size_t actual_recv_count = std::min(chunk_size, count - recv_chunk * chunk_size);
        size_t actual_send_bytes = actual_send_count * type_size;
        size_t actual_recv_bytes = actual_recv_count * type_size;
        
        std::thread send_thread([&rt, data, send_offset, actual_send_bytes]()
        {
            rt.next->Send(data + send_offset, actual_send_bytes);
        });
        
        if (!rt.prev->Recv(data + recv_offset, actual_recv_bytes))
        {
            LOG_ERROR("Rank {}: Failed to recv data from prev for RingAllReduceAllGather in step {}", rank, step);
            send_thread.join();
            return false;
        }
        
        send_thread.join();
    }
    
    return true;
}

bool TopologyRing::AllReduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    // Special for 2 ranks
    if (world_size == 2)
    {
        return AllReduceTwoRanks(comm, send_buf, recv_buf, count, dtype, op);
    }

    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;

    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for AllReduce", rank);
        return false;
    }

    // Copy data from send_buf to recv_buf
    memcpy(recv_buf, send_buf, chunk_bytes);

    // Phase 1: ReduceScatter
    if (!AllReduceReduceScatter(recv_buf, count, dtype, op, rt))
    {
        LOG_ERROR("Rank {}: Failed to AllReduceReduceScatter for AllReduce", rank);
        return false;
    }

    // Phase 2: AllGather
    if (!AllReduceAllGather(recv_buf, count, dtype, rt))
    {
        LOG_ERROR("Rank {}: Failed to AllReduceAllGather for AllReduce", rank);
        return false;
    }

    return true;
}

bool TopologyRing::AllToAll(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    size_t type_size = Utils::GetDataTypeSize(dtype);
    size_t chunk_bytes = count * type_size;
    const char* send_ptr = static_cast<const char*>(send_buf);
    char* recv_ptr = static_cast<char*>(recv_buf);
    
    // Copy data of itself
    memcpy(recv_ptr + rank * chunk_bytes, send_ptr + rank * chunk_bytes, chunk_bytes);
    
    // Simple way: Each rank broadcast its data to other ranks
    for (int src = 0; src < world_size; ++src)
    {
        std::vector<char> temp_buffer(chunk_bytes * world_size);
        
        if (rank == src)
        {
            memcpy(temp_buffer.data(), send_buf, chunk_bytes * world_size);
        }
        
        // Broadcast data from src rank to others
        if (!Broadcast(comm, temp_buffer.data(), count * world_size, dtype, src))
        {
            LOG_ERROR("Rank {}: Failed to Broadcast for AllToAll", rank);
            return false;
        }
        
        // Extract the data needed for current rank
        memcpy(recv_ptr + src * chunk_bytes, temp_buffer.data() + rank * chunk_bytes, chunk_bytes);
    }
    
    return true;
}

bool TopologyRing::Barrier(Communicator& comm)
{
    int dummy = 0;
    RingTransports rt;
    if (!GetRingTransports(comm, rt, world_size))
    {
        LOG_ERROR("Rank {}: Failed to get ring transports for Barrier", rank);
        return false;
    }

    // First ring: clockwise from rank 0, make sure all ranks go to barrier
    if (rank == 0)
    {
        if (!rt.next->Send(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to send to next for Barrier", rank);
            return false;
        }
        if (!rt.prev->Recv(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to recv from prev for Barrier", rank);
            return false;
        }
    }
    else
    {
        if (!rt.prev->Recv(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to recv from prev for Barrier", rank);
            return false;
        }
        if (!rt.next->Send(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to send to next for Barrier", rank);
            return false;
        }
    }

    // Second ring: clockwise from rank 0 to notify barrier is done
    if (rank == 0)
    {
        if (!rt.next->Send(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to send to next for Barrier", rank);
            return false;
        }
        if (!rt.prev->Recv(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to recv from prev for Barrier", rank);
            return false;
        }
    }
    else
    {
        if (!rt.prev->Recv(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to recv from prev for Barrier", rank);
            return false;
        }
        if (!rt.next->Send(&dummy, sizeof(int)))
        {
            LOG_ERROR("Rank {}: Failed to send to next for Barrier", rank);
            return false;
        }
    }

    return true;
}
