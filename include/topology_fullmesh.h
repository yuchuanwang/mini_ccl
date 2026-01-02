// topology_fullmesh.h
// All ranks connect to each other, good for small scale (< 8 ranks)
// 

#pragma once

#include "topology.h"

class TopologyFullMesh : public Topology
{
public:
    TopologyFullMesh()
    {
        topo_name = "FullMesh";
    }
    ~TopologyFullMesh() override = default;

    // Get the neighbors of rank
    std::vector<int> GetNeighbors(int r) const override;
    // Two ranks should connect or not
    bool ShouldConnect(int my_rank, int peer_rank) const override;

    // Collective Communication APIs
    bool Broadcast(Communicator& comm, void* buffer, size_t count, DataType dtype, int root) override;
    bool Gather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root) override;
    bool Scatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root) override;
    bool Reduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root) override;
    bool AllGather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype) override;
    bool ReduceScatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op) override;
    bool AllReduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op) override;
    bool AllToAll(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype) override;

    bool Barrier(Communicator& comm) override;
};
