// utils.cpp
// Utility functions

#include <cstring>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "utils.h"
#include "logger.h"

namespace Utils
{
std::string GetLocalIPAddress()
{
    struct ifaddrs* ifaddr = nullptr;
    std::string ret = "127.0.0.1";
    if (getifaddrs(&ifaddr) == -1)
    {
        LOG_ERROR("Failed to get ifaddrs");
        return ret;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr)
        {
            continue;
        }

        // IPv4 only
        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            char buffer[INET_ADDRSTRLEN];
            struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(addr->sin_addr), buffer, INET_ADDRSTRLEN);

            std::string addr_str(buffer);
            // Ignore loopback
            if (addr_str != "127.0.0.1")
            {
                ret = addr_str;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return ret;
}

// Socket Server
// Use random port to listen if it is 0
// Return fd or -1
int CreateListenSocket(uint16_t port, uint16_t* actual_port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        LOG_ERROR("Failed to create listen socket");
        return -1;
    }

    SetTcpNoDelay(sockfd);
    SetReuseAddr(sockfd);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sockfd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0)
    {
        LOG_ERROR("Failed to bind to port {}", port);
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 128) < 0)
    {
        LOG_ERROR("Failed to listen on port {}", port);
        close(sockfd);
        return -1;
    }

    if (actual_port)
    {
        *actual_port = GetSocketPort(sockfd);
        LOG_INFO("TCP socket listens on port {}", *actual_port);
    }

    return sockfd;
}

int AcceptConnection(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0)
    {
        LOG_ERROR("Failed to accept TCP connection for socket fd {}", listen_fd);
        return -1;
    }

    SetTcpNoDelay(client_fd);

    LOG_DEBUG("Accpet TCP connection from {}:{}", std::string(inet_ntoa(client_addr.sin_addr)), ntohs(client_addr.sin_port));
    return client_fd;
}

// Socket Client
// Return fd or -1
int CreateConnectSocket(const std::string& addr, uint16_t port, int retry_count)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        LOG_ERROR("Failed to create connect socket");
        return -1;
    }

    SetTcpNoDelay(sockfd);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, addr.c_str(), &server_addr.sin_addr) <= 0)
    {
        LOG_ERROR("Invalid address to connect: {}", addr);
        close(sockfd);
        return -1;
    }

    for (int i = 0; i < retry_count; i++)
    {
        if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) == 0)
        {
            // Connected
            LOG_INFO("Cnnect to {}:{}", addr, port);
            return sockfd;
        }

        if (i < retry_count - 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    LOG_ERROR("Failed to connect to {}:{} after {} retries", addr, port, retry_count);
    close(sockfd);
    return -1;
}

// Get the port binding
uint16_t GetSocketPort(int sockfd)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0)
    {
        return ntohs(addr.sin_port);
    }
    else
    {
        LOG_ERROR("Failed to get socket port");
        return -1;
    }
}

// Set TCP_NODELAY to disable Nagle, and reduce latency
void SetTcpNoDelay(int sockfd)
{
    int flag = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
    {
        LOG_WARN("Failed to set socket {} to NODELAY", sockfd);
    }
}

void SetReuseAddr(int sockfd)
{
    int flag = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0)
    {
        LOG_WARN("Failed to set socket {} to SO_REUSEADDR", sockfd);
    }
}

// Send and Recv with length prefix
// 4 bytes of data size + data
bool SendAll(int sockfd, const void* data, size_t size)
{
    // Send size
    uint32_t net_size = htonl(static_cast<uint32_t>(size));
    ssize_t sent = send(sockfd, &net_size, sizeof(uint32_t), 0);
    if (sent != sizeof(uint32_t))
    {
        LOG_ERROR("Failed to send size prefix");
        return false;
    }

    // Send data
    size_t total_sent = 0;
    const char* ptr = static_cast<const char*>(data);
    while(total_sent < size)
    {
        ssize_t n = send(sockfd, ptr + total_sent, size - total_sent, 0);
        if (n <= 0)
        {
            LOG_ERROR("Failed to send data (sent {}/{})", total_sent, size);
            return false;
        }
        total_sent += n;
    }

    return true;
}

bool SendAll(int sockfd, const std::vector<char>& buffer)
{
    return SendAll(sockfd, buffer.data(), buffer.size());
}

bool SendAll(int sockfd, const std::string& str)
{
    return SendAll(sockfd, str.data(), str.size());
}

bool RecvAll(int sockfd, std::vector<char>& buffer)
{
    // Recv size prefix
    uint32_t net_size = 0;
    ssize_t recvd = recv(sockfd, &net_size, sizeof(uint32_t), MSG_WAITALL);
    if (recvd != sizeof(uint32_t))
    {
        LOG_ERROR("Failed to recv size prefix {}/{}", recvd, sizeof(uint32_t));
        return false;
    }

    uint32_t size = ntohl(net_size);
    if (size == 0)
    {
        buffer.clear();
        return true;
    }

    // Recv data
    buffer.resize(size);
    size_t total_recvd = 0;
    while (total_recvd < size)
    {
        ssize_t n = recv(sockfd, buffer.data() + total_recvd, size - total_recvd, MSG_WAITALL);
        if (n <= 0)
        {
            LOG_ERROR("Failed to recv data (recvd {}/{})", total_recvd, size);
            return false;
        }
        total_recvd += n;
    }

    return true;
}

bool RecvAll(int sockfd, std::string& str)
{
    std::vector<char> temp_vec;
    if (!RecvAll(sockfd, temp_vec))
    {
        return false;
    }

    if (!temp_vec.empty())
    {
        str.assign(temp_vec.data(), temp_vec.size());
    }
    else
    {
        str.clear();
    }

    return true;
}

// Encode int to 4 bytes, append to buffer
void EncodeInt(std::vector<char>& buffer, int value)
{
    uint32_t net_value = htonl(static_cast<uint32_t>(value));
    const char* data = reinterpret_cast<const char*>(&net_value);
    buffer.insert(buffer.end(), data, data + sizeof(uint32_t));
}

// Decode int from buffer, append 4 to offset
int DecodeInt(const char* buffer, size_t& offset)
{
    uint32_t net_value;
    memcpy(&net_value, buffer + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    return static_cast<int>(ntohl(net_value));
}

// Encode string to buffer with format: 4 bytes of length + content of string
void EncodeString(std::vector<char>& buffer, const std::string& str)
{
    EncodeInt(buffer, static_cast<int>(str.size()));
    buffer.insert(buffer.end(), str.begin(), str.end());
}

// Decode string from buffer, append 4 + len(str) to offset
std::string DecodeString(const char* buffer, size_t& offset)
{
    int length = DecodeInt(buffer, offset);
    std::string ret(buffer + offset, length);
    offset += length;
    return ret;
}

// Encode NodeInfo to buffer
void EncodeNodeInfo(std::vector<char>& buffer, const NodeInfo& node)
{
    EncodeInt(buffer, node.rank);
    EncodeString(buffer, node.ip_addr);
    EncodeInt(buffer, node.data_port);
}

// TODO: change buffer to vector
// Decode buffer to NodeInfo
NodeInfo DecodeNodeInfo(const char* buffer, size_t& offset)
{
    NodeInfo node;
    node.rank = DecodeInt(buffer, offset);
    node.ip_addr = DecodeString(buffer, offset);
    node.data_port = DecodeInt(buffer, offset);
    return node;
}

// Reduce template
template<typename T>
void ReduceData(const T* send_buf, T* recv_buf, size_t count, ReduceOp op)
{
    switch (op)
    {
    case ReduceOp::SUM:
        for (size_t i = 0; i < count; i++)
        {
            recv_buf[i] += send_buf[i];
        }
        break;

    case ReduceOp::MAX:
        for (size_t i = 0; i < count; i++)
        {
            if (send_buf[i] > recv_buf[i])
            {
                recv_buf[i] = send_buf[i];
            }
        }
        break;

    case ReduceOp::MIN:
        for (size_t i = 0; i < count; i++)
        {
            if (send_buf[i] < recv_buf[i])
            {
                recv_buf[i] = send_buf[i];
            }
        }
        break;

    case ReduceOp::AVG:
        // SUM here, need to divided by world_size after calling this
        for (size_t i = 0; i < count; i++)
        {
            recv_buf[i] += send_buf[i];
        }
        break;
    }
}

template void ReduceData<float>(const float* send_buf, float* recv_buf, size_t count, ReduceOp op);
template void ReduceData<double>(const double* send_buf, double* recv_buf, size_t count, ReduceOp op);
template void ReduceData<int32_t>(const int32_t* send_buf, int32_t* recv_buf, size_t count, ReduceOp op);
template void ReduceData<int64_t>(const int64_t* send_buf, int64_t* recv_buf, size_t count, ReduceOp op);

void PerformReduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int world_size)
{
    switch (dtype)
    {
    case DataType::FLOAT32:
        ReduceData(static_cast<const float*>(send_buf), static_cast<float*>(recv_buf), count, op);
        break;
    case DataType::FLOAT64:
        ReduceData(static_cast<const double*>(send_buf), static_cast<double*>(recv_buf), count, op);
        break;
    case DataType::INT32:
        ReduceData(static_cast<const int32_t*>(send_buf), static_cast<int32_t*>(recv_buf), count, op);
        break;
    case DataType::INT64:
        ReduceData(static_cast<const int64_t*>(send_buf), static_cast<int64_t*>(recv_buf), count, op);
        break;
    
    default:
        LOG_ERROR("Unknown data type to reduce");
        return;
    }

    // Divided by world_size for AVG
    if (op == ReduceOp::AVG)
    {
        switch (dtype)
        {
        case DataType::FLOAT32:
        {
            float* data = static_cast<float*>(recv_buf);
            for (size_t i = 0; i < count; i++)
            {
                data[i] /= world_size;
            }
            break;
        }

        case DataType::FLOAT64:
        {
            double* data = static_cast<double*>(recv_buf);
            for (size_t i = 0; i < count; i++)
            {
                data[i] /= world_size;
            }
            break;
        }

        case DataType::INT32:
        {
            int32_t* data = static_cast<int32_t*>(recv_buf);
            for (size_t i = 0; i < count; i++)
            {
                data[i] /= world_size;
            }
            break;
        }

        case DataType::INT64:
        {
            int64_t* data = static_cast<int64_t*>(recv_buf);
            for (size_t i = 0; i < count; i++)
            {
                data[i] /= world_size;
            }
            break;
        }
        }
    }
}

size_t GetDataTypeSize(DataType dtype)
{
    switch (dtype)
    {
    case DataType::FLOAT32:
        return 4;
    case DataType::FLOAT64:
        return 8;
    case DataType::INT32:
        return 4;
    case DataType::INT64:
        return 8;
    default:
        return 0;
    }
}

const char* GetDataTypeName(DataType dtype)
{
    switch (dtype)
    {
    case DataType::FLOAT32:
        return "FLOAT32";
    case DataType::FLOAT64:
        return "FLOAT64";
    case DataType::INT32:
        return "INT32";
    case DataType::INT64:
        return "INT64";
    default:
        return "UNKNOWN";
    }
}

const char* GetReduceOpName(ReduceOp op)
{
    switch (op)
    {
    case ReduceOp::SUM:
        return "SUM";
    case ReduceOp::MAX:
        return "MAX";
    case ReduceOp::MIN:
        return "MIN";
    case ReduceOp::AVG:
        return "AVG";
    default:
        return "UNKNOWN";
    }
}
} 
