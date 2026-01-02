
#include <thread>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "logger.h"
#include "types.h"
#include "utils.h"
#include "transport_tcp.h"

void socket_server_func(uint16_t port)
{
    TransportTCP server;
    server.Listen(port);

    int client_fd = server.Accept();
    if (client_fd < 0)
    {
        LOG_ERROR("Server: Failed to accept connection");
        close(client_fd);
        return;
    }
    else
    {
        LOG_INFO("Server: Accept connection from client");
        std::string msg_sent = "Hello from Server";
        server.Send((const void*)msg_sent.c_str(), msg_sent.size());
        LOG_INFO("Server: Send '{}' to client", msg_sent);

        close(client_fd);
        return;
    }
}

void socket_client_func(uint16_t port)
{
    TransportTCP client;
    bool ret = client.Connect("127.0.0.1", port);
    if (ret)
    {
        LOG_INFO("Client: Connected to server");

        std::vector<char> buffer;
        buffer.resize(17);
        client.Recv(buffer.data(), 17);

        std::string msg_recv;
        msg_recv.assign(buffer.data(), buffer.size());
        LOG_INFO("Client: Recv '{}' from server", msg_recv);
    }
}

void test_transport_tcp()
{
    uint16_t port = 12345;
    std::thread server_thread(socket_server_func, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread client_thread(socket_client_func, port);

    server_thread.join();
    client_thread.join();
}

int main()
{
    Logger::Instance().SetLogLevel(LogLevel::DEBUG);
    
    test_transport_tcp();

    return 0;
}
