// communicator.cpp
// Communicator for the collective communication
//

#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include "communicator.h"
#include "logger.h"
#include "utils.h"
#include "bootstrap.h"
#include "transport_factory.h"
#include "topology_factory.h"


Communicator::Communicator()
{
}

Communicator::~Communicator()
{
    Finalize();
}

bool Communicator::Init(const CommConfig& cfg, TransportType trans_type, TopologyType topo_type)
{
    config = cfg;
    transport_type = trans_type;
    topology_type = topo_type;

    topology = TopologyFactory::Create(topology_type);
    if (!topology)
    {
        LOG_ERROR("Rank {}: Failed to create topology", config.rank);
        return false;
    }

    if (!topology->Init(config.rank, config.world_size))
    {
        LOG_ERROR("Rank {}: Failed to init Topology", config.rank);
        return false;
    }

    LOG_INFO("Rank {}: Init Communicator world_size = {}, transport = {}, topology = {}", 
        config.rank, config.world_size, TransportFactory::GetTypeName(transport_type), 
        TopologyFactory::GetTypeName(topology_type));

    // Create temp transport to get the available random port for data plane
    auto temp_transport = TransportFactory::Create(transport_type);
    if (!temp_transport)
    {
        LOG_ERROR("Rank {}: Failed to create temp transport to get the available port", config.rank);
        return false;
    }

    if (!temp_transport->Listen(0))
    {
        LOG_ERROR("Rank {}: Failed to listen to get the available port", config.rank);
        return false;
    }

    uint16_t data_port = temp_transport->GetListenPort();
    LOG_INFO("Rank {}: Will use port {} for data plane", config.rank, data_port);
    // Now we can close the temp transport
    temp_transport->Close();

    // Get all nodes info by control plane (Bootstrap)
    Bootstrap bootstrap;
    std::vector<NodeInfo> all_nodes;
    if (!bootstrap.Run(config, data_port, all_nodes))
    {
        LOG_ERROR("Rank {}: Failed to run bootstrap", config.rank);
        return false;
    }
    LOG_INFO("Rank {}: Bootstrap is ready, get {} nodes info", config.rank, all_nodes.size());

    // Establish connections according to topology
    if (!InitTransports(all_nodes))
    {
        LOG_ERROR("Rank {}: Failed to init transports for all nodes", config.rank);
        return false;
    }

    LOG_INFO("Rank {}: Communicator init successfully", config.rank);
    return true;
}

void Communicator::Finalize()
{
    for (auto& pair : transports)
    {
        pair.second->Close();
    }
    transports.clear();
    LOG_INFO("Rank {}: Communicator finalized", config.rank);
}

// Get transport to peer rank
std::shared_ptr<Transport> Communicator::GetTransport(int peer_rank) const
{
    auto it = transports.find(peer_rank);
    if (it != transports.end())
    {
        return it->second;
    }
    else
    {
        return nullptr;
    }
}

// Collective Communication APIs
bool Communicator::Send(const void* send_buf, size_t count, DataType dtype, int dest)
{
    if (dest == config.rank)
    {
        LOG_ERROR("Rank {}: Cannot send to itself", config.rank);
        return false;
    }

    auto transport = GetTransport(dest);
    if (!transport)
    {
        LOG_ERROR("Rank {}: Failed to get transport to {}", config.rank, dest);
        return false;
    }

    size_t dtype_size = Utils::GetDataTypeSize(dtype);
    size_t total_size = count * dtype_size;
    LOG_DEBUG("Rank {}: Send {} bytes from {} to {}", config.rank, total_size, dest);

    if (!transport->Send(send_buf, total_size))
    {
        LOG_ERROR("Rank {}: Failed to send {} bytes to {}", config.rank, total_size, dest);
        return false;
    }
    return true;
}

bool Communicator::Recv(void* recv_buf, size_t count, DataType dtype, int source)
{
    if (source == config.rank)
    {
        LOG_ERROR("Rank {}: Cannot recv from itself", config.rank);
        return false;
    }

    auto transport = GetTransport(source);
    if (!transport)
    {
        LOG_ERROR("Rank {}: Failed to get transport from {}", config.rank, source);
        return false;
    }

    size_t dtype_size = Utils::GetDataTypeSize(dtype);
    size_t total_size = count * dtype_size;
    LOG_DEBUG("Rank {}: Recv {} bytes from {}", config.rank, total_size, source);

    if (!transport->Recv(recv_buf, total_size))
    {
        LOG_ERROR("Rank {}: Failed to recv {} bytes from {}", config.rank, total_size, source);
        return false;
    }
    return true;
}

bool Communicator::Broadcast(void* buffer, size_t count, DataType dtype, int root)
{
    LOG_DEBUG("Rank {}: Broadcast from root {}, count {}, dtype {}", config.rank, root, count, Utils::GetDataTypeName(dtype));
    return topology->Broadcast(*this, buffer, count, dtype, root);
}

bool Communicator::Gather(const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    LOG_DEBUG("Rank {}: Gather to root {}, count {}, dtype {}", config.rank, root, count, Utils::GetDataTypeName(dtype));
    return topology->Gather(*this, send_buf, recv_buf, count, dtype, root);
}

bool Communicator::Scatter(const void* send_buf, void* recv_buf, size_t count, DataType dtype, int root)
{
    LOG_DEBUG("Rank {}: Scatter from root {}, count {}, dtype {}", config.rank, root, count, Utils::GetDataTypeName(dtype));
    return topology->Scatter(*this, send_buf, recv_buf, count, dtype, root);
}

bool Communicator::Reduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int root)
{
    LOG_DEBUG("Rank {}: Reduce to root {}, count {}, dtype {}, op {}", config.rank, root, count, Utils::GetDataTypeName(dtype), Utils::GetReduceOpName(op));
    return topology->Reduce(*this, send_buf, recv_buf, count, dtype, op, root);
}

bool Communicator::AllGather(const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    LOG_DEBUG("Rank {}: AllGather count {}, dtype {}", config.rank, count, Utils::GetDataTypeName(dtype));
    return topology->AllGather(*this, send_buf, recv_buf, count, dtype);
}

bool Communicator::ReduceScatter(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    LOG_DEBUG("Rank {}: ReduceScatter count {}, dtype {}, op {}", config.rank, count, Utils::GetDataTypeName(dtype), Utils::GetReduceOpName(op));
    return topology->ReduceScatter(*this, send_buf, recv_buf, count, dtype, op);
}

bool Communicator::AllReduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op)
{
    LOG_DEBUG("Rank {}: AllReduce count {}, dtype {}, op {}", config.rank, count, Utils::GetDataTypeName(dtype), Utils::GetReduceOpName(op));
    return topology->AllReduce(*this, send_buf, recv_buf, count, dtype, op);
}

bool Communicator::AllToAll(const void* send_buf, void* recv_buf, size_t count, DataType dtype)
{
    LOG_DEBUG("Rank {}: AllToAll count {}, dtype {}", config.rank, count, Utils::GetDataTypeName(dtype));
    return topology->AllToAll(*this, send_buf, recv_buf, count, dtype);
}

bool Communicator::Barrier()
{
    LOG_DEBUG("Rank {}: Barrier", config.rank);
    return topology->Barrier(*this);
}

bool Communicator::InitTransports(const std::vector<NodeInfo>& all_nodes)
{
    // Get neighbors to be connected
    std::vector<int> neighbors = topology->GetNeighbors(config.rank);
    LOG_INFO("Rank {}: Topology {} needs connections to {} neighbors", config.rank, topology->GetName(), neighbors.size());

    // I will connect to these peers
    std::vector<int> connect_peers;
    // These peers will connect to me
    std::vector<int> accept_peers;
    for (int peer : neighbors)
    {
        if (topology->ShouldConnect(config.rank, peer))
        {
            connect_peers.push_back(peer);
        }
        else
        {
            accept_peers.push_back(peer);
        }
    }
    LOG_INFO("Rank {}: will actively connect to {} peers, passively accept {} peers", config.rank, connect_peers.size(), accept_peers.size());

    const NodeInfo& my_info = all_nodes[config.rank];
    // Create listen transport for accept peers
    std::shared_ptr<Transport> listen_transport;
    if (!accept_peers.empty())
    {
        listen_transport = TransportFactory::Create(transport_type);
        if (!listen_transport || !listen_transport->Listen(my_info.data_port))
        {
            LOG_ERROR("Rank {}: Failed to create listen transport on port {}", config.rank, my_info.data_port);
            return false;
        }
        LOG_INFO("Rank {}: Listening on port {} for {} accept peers", config.rank, my_info.data_port, accept_peers.size());
    }

    // Connect and Accept in threads
    std::atomic<bool> error_occurred(false);
    std::thread connect_thread([&]()
    {
        if (!connect_peers.empty())
        {
            if (!ConnectActivePeers(all_nodes, connect_peers, error_occurred))
            {
                error_occurred = true;
            }
        }
    });
    std::thread accept_thread([&]()
    {
        if (!accept_peers.empty() && listen_transport)
        {
            if (!AcceptPassivePeers(listen_transport, accept_peers, error_occurred))
            {
                error_occurred = true;
            }
        }
    });
    connect_thread.join();
    accept_thread.join();
    if (error_occurred)
    {
        return false;
    }

    // Send current rank to the peers I connect to
    // The value will be recved in AcceptPassivePeers
    for (int peer : connect_peers)
    {
        if (!SendOrFail(transports[peer].get(), &config.rank, sizeof(int), peer, "InitTransports"))
        {
            return false;
        }
    }

    LOG_INFO("Rank {}: All transports are init, with {} connect + {} accept = {} connections", 
        config.rank, connect_peers.size(), accept_peers.size(), transports.size());
    return true;
}

bool Communicator::ConnectActivePeers(const std::vector<NodeInfo>& all_nodes, const std::vector<int>& connect_peers, std::atomic<bool>& error_occured)
{
    std::mutex transport_mutex;
    std::vector<std::thread> threads;

    for (int peer : connect_peers)
    {
        threads.emplace_back([&, peer]()
        {
            if (error_occured)
            {
                return;
            }

            const auto& peer_info = all_nodes[peer];
            auto transport = TransportFactory::Create(transport_type);
            if (!transport)
            {
                LOG_ERROR("Rank {}: Failed to create transport for connecting to rank {}", config.rank, peer);
                error_occured = true;
                return;
            }

            bool connected = false;
            for (int retry = 0; retry < 30; ++retry)
            {
                if (transport->Connect(peer_info.ip_addr, peer_info.data_port))
                {
                    connected = true;
                    break;
                }

                LOG_DEBUG("Rank {}: Failed to connect to rank {}, retry {}/30", config.rank, peer, retry);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }

            if (!connected)
            {
                LOG_ERROR("Rank {}: Failed to connect to rank {}:{} after 30 reties", config.rank, peer_info.ip_addr, peer_info.data_port);
                error_occured = true;
                return;
            }

            {
                std::lock_guard<std::mutex> lock(transport_mutex);
                transports[peer] = transport;
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

    return !error_occured;
}

bool Communicator::AcceptPassivePeers(const std::shared_ptr<Transport>& listen_transport, const std::vector<int>& accept_peers, std::atomic<bool>& error_occured)
{
    std::mutex transports_mutex;
    std::vector<std::thread> threads;

    for (size_t i = 0; i < accept_peers.size(); ++i)
    {
        threads.emplace_back([&, i]()
        {
            if (error_occured)
            {
                return;
            }

            auto transport = listen_transport->CreateAcceptedConnection();
            if (!transport)
            {
                LOG_ERROR("Rank {}: Failed to accept connection for {}/{}", config.rank, i+1, accept_peers.size());
                error_occured = true;
                return;
            }

            // Accept peer rank
            // The rank was sent in InitTransports
            int peer_rank = -1;
            if (!RecvOrFail(transport.get(), &peer_rank, sizeof(int), -1, "AcceptPassivePeers"))
            {
                error_occured = true;
                return;
            }

            {
                std::lock_guard<std::mutex> lock(transports_mutex);
                transports[peer_rank] = transport;
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

    return !error_occured;
}

bool Communicator::SendOrFail(Transport* transport, const void* data, size_t size, int peer, const char* context)
{
    if (!transport->Send(data, size))
    {
        LOG_ERROR("Rank {}: {} - Failed to send to rank {}", config.rank, context, peer);
        return false;
    }
    return true;
}

bool Communicator::RecvOrFail(Transport* transport, void* data, size_t size, int peer, const char* context)
{
    if (!transport->Recv(data, size))
    {
        LOG_ERROR("Rank {}: {} - Failed to recv from rank {}", config.rank, context, peer);
        return false;
    }
    LOG_INFO("Rank {}: {} - Recv {} from rank {}", config.rank, context, size, peer);
    return true;
}

void Communicator::EnsureBufferSize(std::vector<char>& buffer, size_t required_size)
{
    if (buffer.size() < required_size)
    {
        buffer.resize(required_size);
    }
}
