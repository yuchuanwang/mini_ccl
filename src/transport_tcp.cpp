// transport_tcp.cpp
// Using TCP to send/recv data
// 

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "transport_tcp.h"
#include "logger.h"
#include "utils.h"

TransportTCP::TransportTCP()
{
}

TransportTCP::~TransportTCP()
{
    Close();
}

bool TransportTCP::Listen(uint16_t port)
{
    listen_fd = Utils::CreateListenSocket(port, &listen_port);
    if (listen_fd < 0)
    {
        return false;
    }

    return true;
}

bool TransportTCP::Accept()
{
    if (listen_fd < 0)
    {
        LOG_ERROR("Not in listen mode");
        return false;
    }

    sockfd = Utils::AcceptConnection(listen_fd);
    if (sockfd < 0)
    {
        return false;
    }

    connected = true;
    return true;
}

// Return an instance of Transport
std::shared_ptr<Transport> TransportTCP::CreateAcceptedConnection()
{
    if (listen_fd < 0)
    {
        LOG_ERROR("Not in listen mode");
        return nullptr;
    }

    int client_fd = Utils::AcceptConnection(listen_fd);
    if (client_fd < 0)
    {
        return nullptr;
    }

    // Create new TransportTCP
    auto new_transport = std::make_shared<TransportTCP>();
    new_transport->SetSocket(client_fd);
    return new_transport;
}

bool TransportTCP::Connect(const std::string& addr, uint16_t port)
{
    sockfd = Utils::CreateConnectSocket(addr, port, 1);
    if (sockfd < 0)
    {
        return false;
    }

    connected = true;
    return true;
}

bool TransportTCP::Send(const void* data, size_t size)
{
    if (!connected || sockfd < 0)
    {
        LOG_ERROR("Not connected to send");
        return false;
    }

    return SendRaw(data, size);
}

bool TransportTCP::Recv(void* data, size_t size)
{
    if (!connected || sockfd < 0)
    {
        LOG_ERROR("Not connected to recv");
        return false;
    }

    return RecvRaw(data, size);
}

void TransportTCP::Close()
{
    if (sockfd >= 0)
    {
        close(sockfd);
        sockfd = -1;
    }
    if (listen_fd >= 0)
    {
        close(listen_fd);
        listen_fd = -1;
    }
    connected = false;
}

// Create transport from existing socket fd
void TransportTCP::SetSocket(int fd)
{
    sockfd = fd;
    connected = true;
}

bool TransportTCP::SendRaw(const void* data, size_t size)
{
    const char* buffer = static_cast<const char*>(data);
    size_t total = 0;
    while(total < size)
    {
        ssize_t sent = send(sockfd, buffer + total, size - total, 0);
        if (sent < 0)
        {
            LOG_ERROR("Failed to send");
            return false;
        }
        else if (sent == 0)
        {
            LOG_INFO("Connection closed by peer when send");
            return false;
        }

        total += sent;
    }

    return true;
}

bool TransportTCP::RecvRaw(void* data, size_t size)
{
    char* buffer = static_cast<char*>(data);
    size_t total = 0;
    while (total < size)
    {
        ssize_t recvd = recv(sockfd, buffer + total, size - total, 0);
        if (recvd < 0)
        {
            LOG_ERROR("Failed to recv");
            return false;
        }
        else if (recvd == 0)
        {
            LOG_INFO("Connection close by peer when recv");
            return false;
        }

        total += recvd;
    }

    return true;
}
