// bootstrap.h
// Control Plane, using TCP to gather and broadcast all nodes info
// Rank 0 (master) collect all nodes info, then broadcast to all other nodes
//

#pragma once

#include <vector>
#include <memory>
#include "types.h"

class TransportTCP;
class Bootstrap
{
public:
    Bootstrap() = default;
    ~Bootstrap() = default;

    // Collect and return all nodes info
    bool Run(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes);

private:
    bool RunMaster(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes);
    bool RunWorker(const CommConfig& config, uint16_t data_port, std::vector<NodeInfo>& all_nodes);

    bool AcceptConnections(int listen_fd, int count, std::vector<int>& client_fds);
    void CloseAllSockets(const std::vector<int>& socks);

    bool RecvNodeInfo(int sockfd, NodeInfo& node_info);
    bool SendNodeInfo(int sockfd, const NodeInfo& node_info);
    bool BroadcastAllNodes(const std::vector<int>& socks, const std::vector<NodeInfo>& nodes);
    bool RecvAllNodes(int sockfd, std::vector<NodeInfo>& nodes);

private:
    std::vector<std::shared_ptr<TransportTCP>> connections;
};
