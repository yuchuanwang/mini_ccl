// transport_tcp.h
// Using TCP to send/recv data
// 

#pragma once

#include "transport.h"

class TransportTCP : public Transport
{
public:
    TransportTCP();
    ~TransportTCP() override;

    bool Listen(uint16_t port) override;
    uint16_t GetListenPort() const override
    {
        return listen_port;
    }

    bool Accept() override;
    // Return an instance of Transport
    std::shared_ptr<Transport> CreateAcceptedConnection() override;

    bool Connect(const std::string& addr, uint16_t port) override;

    bool Send(const void* data, size_t size) override;
    bool Recv(void* data, size_t size) override;

    void Close() override;
    bool IsConnected() const override
    {
        return connected;
    }

    // Create transport from existing socket fd
    void SetSocket(int fd);

    int GetListenFd() const
    {
        return listen_fd;
    }

private:
    // Send/Recv without length prefix
    bool SendRaw(const void* data, size_t size);
    bool RecvRaw(void* data, size_t size);

private:
    int sockfd = -1;
    int listen_fd = -1;
    uint16_t listen_port = 0;
    bool connected = false;
};
