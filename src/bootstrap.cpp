// bootstrap.cpp
// Control Plane, using TCP to gather and broadcast all nodes info
// Rank 0 (master) collect all nodes info, then broadcast to all other nodes
//

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "bootstrap.h"
#include "utils.h"
#include "logger.h"
#include "transport_tcp.h"

// Collect and return all nodes info
bool Bootstrap::Run(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes)
{
    if (config.rank == 0)
    {
        return RunMaster(config, data_port, all_nodes);
    }
    else
    {
        return RunWorker(config, data_port, all_nodes);
    }
}

bool Bootstrap::RunMaster(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes)
{
    LOG_INFO("Bootstrap - Master: Running as master with world size {}", config.world_size);

    // Prepare all nodes
    all_nodes.resize(config.world_size);
    // Fill master info
    // Using localhost/127.0.0.1 for multi-threads, or single node
    std::string local_ip = (config.master_addr == "127.0.0.1") ? "127.0.0.1" : Utils::GetLocalIPAddress();
    all_nodes[0] = NodeInfo(0, local_ip, data_port);
    LOG_INFO("Bootstrap - Master: Node Info - rank = 0, IP = {}, port = {}", local_ip, data_port);

    // Create listen socket
    int listen_fd = Utils::CreateListenSocket(config.master_port);
    if (listen_fd < 0)
    {
        LOG_ERROR("Bootstrap - Master: Failed to create listen socket on port {}", config.master_port);
        return false;
    }
    LOG_INFO("Bootstrap - Master: Listening on port {}", config.master_port);

    // Accept connections from all workers
    std::vector<int> worker_socks;
    if (!AcceptConnections(listen_fd, config.world_size - 1, worker_socks))
    {
        LOG_ERROR("Bootstrap - Master: Failed to accept connections from worker ranks");
        close(listen_fd);
        return false;
    }

    // Recv NodeInfo from each worker rank
    for (size_t i = 0; i < worker_socks.size(); ++i)
    {
        NodeInfo node_info;
        if (!RecvNodeInfo(worker_socks[i], node_info))
        {
            LOG_ERROR("Bootstrap - Master: Failed to recv Node Info from worker rank {}", i);
            CloseAllSockets(worker_socks);
            close(listen_fd);
            return false;
        }

        all_nodes[node_info.rank] = node_info;
        LOG_INFO("Bootstrap - Master: Recv Node Info - rank = {}, IP = {}, port = {}", node_info.rank, node_info.ip_addr, node_info.data_port);
    }

    // Now broadcast all nodes to workers
    if (!BroadcastAllNodes(worker_socks, all_nodes))
    {
        LOG_ERROR("Bootstrap - Master: Failed to broadcast all Node Info to worker ranks");
        CloseAllSockets(worker_socks);
        close(listen_fd);
        return false;
    }

    LOG_INFO("Bootstrap - Master: Finish collecting and broadcasting all Node Info to worker ranks");
    CloseAllSockets(worker_socks);
    close(listen_fd);
    return true;
}

bool Bootstrap::RunWorker(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes)
{
    LOG_INFO("Bootstrap - Worker: Running as worker {}", config.rank);

    // Connect to master
    int sockfd = Utils::CreateConnectSocket(config.master_addr, config.master_port, 30);
    if (sockfd < 0)
    {
        LOG_ERROR("Bootstrap - Worker: Failed to connect to master {}:{}", config.master_addr, config.master_port);
        return false;
    }
    LOG_INFO("Bootstrap - Worker: Connect to master {}:{}", config.master_addr, config.master_port);

    // Send Node Info
    std::string local_ip = (config.master_addr == "127.0.0.1") ? "127.0.0.1" : Utils::GetLocalIPAddress();
    NodeInfo node_info(config.rank, local_ip, data_port);
    if (!SendNodeInfo(sockfd, node_info))
    {
        LOG_ERROR("Bootstrap - Worker: rank {} failed to send Node Info to master", config.rank);
        close(sockfd);
        return false;
    }
    LOG_INFO("Bootstrap - Worker: Send Node Info to master - rank = {}, IP = {}, port = {}", config.rank, local_ip, data_port);

    // Recv all Node Info
    if (!RecvAllNodes(sockfd, all_nodes))
    {
        LOG_ERROR("Bootstrap - Worker: rank {} failed to recv all Node Info from master", config.rank);
        close(sockfd);
        return false;
    }

    LOG_INFO("Bootstrap - Worker: rank {} recv all Node Info from master for {} nodes", config.rank, all_nodes.size());
    close(sockfd);
    return true;
}

bool Bootstrap::AcceptConnections(int listen_fd, int count, std::vector<int>& client_fds)
{
    client_fds.clear();
    client_fds.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0)
        {
            LOG_ERROR("Bootstrap: Failed to accept connection {}", i);
            CloseAllSockets(client_fds);
            return false;
        }

        client_fds.push_back(client_fd);
        LOG_INFO("Bootstrap: Accept connection {}, fd = {}", i, client_fd);
    }

    return true;
}

void Bootstrap::CloseAllSockets(const std::vector<int>& socks)
{
    for (int sockfd : socks)
    {
        close(sockfd);
    }
}

bool Bootstrap::RecvNodeInfo(int sockfd, NodeInfo& node_info)
{
    std::vector<char> buffer;
    if (!Utils::RecvAll(sockfd, buffer))
    {
        LOG_ERROR("Bootstrap: Failed to recv Node Info");
        return false;
    }

    size_t offset = 0;
    node_info = Utils::DecodeNodeInfo(buffer.data(), offset);

    LOG_INFO("Bootstrap: Recv Node Info - rank = {}, IP = {}, port = {}", node_info.rank, node_info.ip_addr, node_info.data_port);
    return true;
}

bool Bootstrap::SendNodeInfo(int sockfd, const NodeInfo& node_info)
{
    std::vector<char> buffer;
    Utils::EncodeNodeInfo(buffer, node_info);

    LOG_INFO("Test: NodeInfo size {}", buffer.size());

    if (!Utils::SendAll(sockfd, buffer))
    {
        LOG_ERROR("Bootstrap: Failed to send Node Info");
        return false;
    }

    LOG_INFO("Bootstrap: Send Node Info - rank = {}, IP = {}, port = {}", node_info.rank, node_info.ip_addr, node_info.data_port);
    return true;
}

bool Bootstrap::BroadcastAllNodes(const std::vector<int>& socks, const std::vector<NodeInfo>& nodes)
{
    // Encode all nodes
    std::vector<char> buffer;
    Utils::EncodeInt(buffer, static_cast<int>(nodes.size()));
    for (const auto& node : nodes)
    {
        Utils::EncodeNodeInfo(buffer, node);
    }

    // Send to all sockets
    for (int sockfd : socks)
    {
        if (!Utils::SendAll(sockfd, buffer))
        {
            LOG_ERROR("Bootstrap: Failed to broadcast all nodes to socket {}", sockfd);
            return false;
        }

        LOG_INFO("Bootstrap: Broadcast all nodes to socket {}", sockfd);
    }

    return true;
}

bool Bootstrap::RecvAllNodes(int sockfd, std::vector<NodeInfo>& nodes)
{
    std::vector<char> buffer;
    if (!Utils::RecvAll(sockfd, buffer))
    {
        LOG_ERROR("Bootstrap: Failed to recv all nodes from socket {}", sockfd);
        return false;
    }

    // Decode all nodes
    size_t offset = 0;
    int num_nodes = Utils::DecodeInt(buffer.data(), offset);
    nodes.resize(num_nodes);

    for (int i = 0; i < num_nodes; ++i)
    {
        nodes[i] = Utils::DecodeNodeInfo(buffer.data(), offset);
        LOG_INFO("Bootstrap: Recv All Node Info - rank = {}, IP = {}, port = {}", nodes[i].rank, nodes[i].ip_addr, nodes[i].data_port);
    }

    return true;
}
