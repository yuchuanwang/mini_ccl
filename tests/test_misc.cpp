
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

void test_GetLocalIPAddress()
{
    std::string local_addr = Utils::GetLocalIPAddress();
    LOG_INFO("Local IP Address: {}", local_addr);
    return;
}

void socket_server_func(uint16_t port)
{
    int server_sock = Utils::CreateListenSocket(port);
    if (server_sock < 0)
    {
        LOG_ERROR("Failed to create server socket");
        return;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0)
    {
        LOG_ERROR("Server: Failed to accept connection");
        close(server_sock);
        return;
    }
    else
    {
        uint16_t client_port = Utils::GetSocketPort(client_sock);
        LOG_INFO("Server: Accept client connection from port {}", client_port);

        std::string msg_sent = "Hello from Server";
        Utils::SendAll(client_sock, msg_sent);
        LOG_INFO("Server: Send data to client - {}", msg_sent);

        std::string msg_recvd;
        Utils::RecvAll(client_sock, msg_recvd);
        LOG_INFO("Server: Recv data from client - {}", msg_recvd);

        close(client_sock);
        close(server_sock);
        return;
    }
}

void socket_client_func(uint16_t port)
{
    int client_sock = Utils::CreateConnectSocket("127.0.0.1", port);
    if (client_sock < 0)
    {
        LOG_ERROR("Client: Failed to create socket");
        return;
    }
    else
    {
        uint16_t client_port = Utils::GetSocketPort(client_sock);
        LOG_INFO("Client: Connect to socket server by client port {}", client_port);

        std::string msg_recvd;
        Utils::RecvAll(client_sock, msg_recvd);
        LOG_INFO("Client: Recv data from server - {}", msg_recvd);

        std::string msg_sent = "Hello from Client";
        Utils::SendAll(client_sock, msg_sent);
        LOG_INFO("Client: Send data to server - {}", msg_sent);

        close(client_sock);
        return;
    }
}

void test_socket_connection()
{
    uint16_t port = 12345;
    std::thread server_thread(socket_server_func, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread client_thread(socket_client_func, port);

    server_thread.join();
    client_thread.join();
}

void test_socket_connection2()
{
    uint16_t acutal_port = 0;
    int listen_fd = Utils::CreateListenSocket(0, &acutal_port);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread client_thread(socket_client_func, acutal_port);

    int client_fd = Utils::AcceptConnection(listen_fd);

    std::string msg_sent = "Hello from Server";
    Utils::SendAll(client_fd, msg_sent);
    LOG_INFO("Server: Send data to client - {}", msg_sent);

    std::string msg_recvd;
    Utils::RecvAll(client_fd, msg_recvd);
    LOG_INFO("Server: Recv data from client - {}", msg_recvd);

    client_thread.join();

    close(listen_fd);
    close(client_fd);
}

void test_encode_decode()
{
    NodeInfo node {1, "127.0.0.1", 12345};
    LOG_INFO("Encoded NodeInfo {} {} {}", node.rank, node.ip_addr, node.data_port);

    std::vector<char> buffer;
    Utils::EncodeNodeInfo(buffer, node);

    size_t offset = 0;
    NodeInfo node2 = Utils::DecodeNodeInfo(buffer.data(), offset);
    LOG_INFO("Decoded NodeInfo {} {} {}", node2.rank, node2.ip_addr, node2.data_port);
}

int main()
{
    Logger::Instance().SetLogLevel(LogLevel::DEBUG);
    
    test_encode_decode();

    test_GetLocalIPAddress();
    test_socket_connection();
    test_socket_connection2();

    return 0;
}
