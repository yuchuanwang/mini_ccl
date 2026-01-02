// topology.h
// Base class of topology for the collective communication
//

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <cstddef>
#include "types.h"

class Communicator;

class Topology
{
public:
    virtual ~Topology() = default;

    // Init with rank and world size
    virtual bool Init(int r, int ws)
    {
        rank = r;
        world_size = ws;
        return true;
    }

    // Get the neighbors of rank
    virtual std::vector<int> GetNeighbors(int r) const = 0;
    // Two ranks should connect or not
    virtual bool ShouldConnect(int my_rank, int peer_rank) const = 0;

    const char* GetName() const
    {
        return topo_name.c_str();
    }

    // Collective Communication API
    virtual bool Broadcast(Communicator& comm, void* buffer, size_t count, DataType dtype, int root) = 0;
    virtual bool Gather(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype, int root) = 0;
    virtual bool Scatter(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype, int root) = 0;
    virtual bool Reduce(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype, ReduceOp op, int root) = 0;
    virtual bool AllGather(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype) = 0;
    virtual bool ReduceScatter(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype, ReduceOp op) = 0;
    virtual bool AllReduce(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype, ReduceOp op) = 0;
    virtual bool AllToAll(Communicator& comm, const void* send_buf, void* recv_buf, 
        size_t count, DataType dtype) = 0;

    virtual bool Barrier(Communicator& comm) = 0;
    

protected:
    int rank = -1;
    int world_size = 0;
    std::string topo_name;
};
