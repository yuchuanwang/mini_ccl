// communicator.h
// Communicator for the collective communication
// 

#pragma once

#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include "types.h"

class Transport;
class Topology;

class Communicator
{
public:
    Communicator();
    ~Communicator();

    bool Init(const CommConfig& cfg, TransportType trans_type, TopologyType topo_type);
    void Finalize();

    // Get Config
    int GetRank() const
    {
        return config.rank;
    }
    int GetWorldSize() const
    {
        return config.world_size;
    }
    // Get transport to peer rank
    std::shared_ptr<Transport> GetTransport(int peer_rank) const;

    // Testing
    const std::map<int, std::shared_ptr<Transport>>& GetTransports() const
    {
        return transports;
    }

    // Collective Communication APIs
    bool Send(const void* send_buf, size_t count, DataType dtype, int dest);
    bool Recv(void* recv_buf, size_t count, DataType dtype, int source);
    bool Broadcast(void* buffer, size_t count, DataType dtype, int root);
    bool Gather(const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root);
    bool Scatter(const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root);
    bool Reduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root);
    bool AllGather(const void* send_buf, void* recv_buf, size_t count, DataType dtype);
    bool ReduceScatter(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op);
    bool AllReduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op);
    bool AllToAll(const void* send_buf, void* recv_buf, size_t count, DataType dtype);
    bool Barrier();

private:
    bool InitTransports(const std::vector<NodeInfo>& all_nodes);
    bool ConnectActivePeers(const std::vector<NodeInfo>& all_nodes, const std::vector<int>& connect_peers, std::atomic<bool>& error_occured);
    bool AcceptPassivePeers(const std::shared_ptr<Transport>& listen_transport, const std::vector<int>& accept_peers, std::atomic<bool>& error_occured);

    bool SendOrFail(Transport* transport, const void* data, size_t size, int peer, const char* context);
    bool RecvOrFail(Transport* transport, void* data, size_t size, int peer, const char* context);

    void EnsureBufferSize(std::vector<char>& buffer, size_t required_size);

private:
    CommConfig config;
    TransportType transport_type = TransportType::TCP;
    TopologyType topology_type = TopologyType::FULL_MESH;

    std::shared_ptr<Topology> topology;
    // One transport for each peer, rank -> transport
    std::map<int, std::shared_ptr<Transport>> transports;

    // Reuse buffer to avoid memory fragment
    std::vector<char> temp_buffer;
};
