// utils.h
// Utility functions

#pragma once

#include <string>
#include <vector>
#include "types.h"

namespace Utils
{
    std::string GetLocalIPAddress();

    // Socket Server
    // If port is 0, use random port, and save it to actual_port
    // Return fd or -1
    int CreateListenSocket(uint16_t port, uint16_t* actual_port = nullptr);

    int AcceptConnection(int listen_fd);

    // Socket Client
    // Return fd or -1
    int CreateConnectSocket(const std::string& addr, uint16_t port, int retry_count=30);

    // Get the port binding
    uint16_t GetSocketPort(int sockfd);

    // Set TCP_NODELAY to disable Nagle, and reduce latency
    void SetTcpNoDelay(int sockfd);

    // Set SO_REUSEADDR to socket, so that we can reuse port in state TIME_WAIT
    void SetReuseAddr(int sockfd);

    // Send and Recv with length prefix
    // 4 bytes of data size + data
    bool SendAll(int sockfd, const void* data, size_t size);
    bool SendAll(int sockfd, const std::vector<char>& buffer);
    bool SendAll(int sockfd, const std::string& str);
    bool RecvAll(int sockfd, std::vector<char>& buffer);
    bool RecvAll(int sockfd, std::string& str);

    // Encode int to 4 bytes, append to buffer
    void EncodeInt(std::vector<char>& buffer, int value);
    // Decode int from buffer, append 4 to offset
    int DecodeInt(const char* buffer, size_t& offset);

    // Encode string to buffer with format: 4 bytes of length + content of string
    void EncodeString(std::vector<char>& buffer, const std::string& str);
    // Decode string from buffer, append 4 + len(str) to offset
    std::string DecodeString(const char* buffer, size_t& offset);

    // Encode NodeInfo to buffer
    void EncodeNodeInfo(std::vector<char>& buffer, const NodeInfo& node);
    // Decode buffer to NodeInfo
    NodeInfo DecodeNodeInfo(const char* buffer, size_t& offset);

    // Reduce template
    template<typename T>
    void ReduceData(const T* send_buf, T* recv_buf, size_t count, ReduceOp op);
    void PerformReduce(const void* send_buf, void* recv_buf, size_t count, DataType dtype, ReduceOp op, int world_size);

    // Align memory
    inline size_t AlignUp(size_t size, size_t alignment)
    {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    size_t GetDataTypeSize(DataType dtype);
    const char* GetDataTypeName(DataType dtype);
    const char* GetReduceOpName(ReduceOp op);
};
