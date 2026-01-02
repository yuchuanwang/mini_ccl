// topology_ring.h
// All ranks connect as a ring, good for middle scale
// 

#pragma once

#include "topology.h"

class Transport;

class TopologyRing : public Topology
{
public:
    TopologyRing()
    {
        topo_name = "Ring";
    }
    ~TopologyRing() override = default;

    // Get the neighbors of rank
    std::vector<int> GetNeighbors(int r) const override;
    // Two ranks should connect or not
    bool ShouldConnect(int my_rank, int peer_rank) const override;

    // Collective Communication API
    bool Broadcast(Communicator& comm, void* buffer, size_t count, DataType dtype, int root) override;
    bool Gather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root) override;
    bool Scatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root) override;
    bool Reduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root) override;
    bool AllGather(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype) override;
    bool ReduceScatter(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op) override;
    bool AllReduce(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op) override;
    bool AllToAll(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype) override;

    bool Barrier(Communicator& comm) override;

    // Ring specific
    int GetPrevRank(int r) const;
    int GetNextRank(int r) const;

private:
    struct RingTransports
    {
        std::shared_ptr<Transport> prev;
        std::shared_ptr<Transport> next;
        int prev_rank;
        int next_rank;
    };

private:
    bool GetRingTransports(Communicator& comm, RingTransports& rt, int world_size);
    bool AllReduceTwoRanks(Communicator& comm, const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op);
    bool AllReduceReduceScatter(void* recv_buf, size_t count, DataType dtype, ReduceOp op, RingTransports& rt);
    bool AllReduceAllGather(void* recv_buf, size_t count, DataType dtype, RingTransports& rt);
};
