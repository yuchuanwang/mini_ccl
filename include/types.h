// types.h
// Define the types used in this project

#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

// Datatypes supported in collective communication
enum class DataType
{
    FLOAT32,
    FLOAT64,
    INT32,
    INT64
};

// Operations supported in Reduce/AllReduce
enum class ReduceOp
{
    SUM,
    MAX,
    MIN,
    AVG
};

// Topologies supported in collective communication
enum class TopologyType
{
    FULL_MESH,
    RING
};

// Transport supported in collective communication data plane
enum class TransportType
{
    TCP,
    RDMA
};

// Configuration for collective communication control plane
struct CommConfig
{
    int rank = 0;
    int world_size = 1;
    // These are the IP and port for Bootstrap/Control plane
    std::string master_addr = "127.0.0.1";
    uint16_t master_port = 12345;
};

struct NodeInfo
{
    int rank = 0;
    // These are the IP and port for data plane
    std::string ip_addr;
    uint16_t data_port = 0;

    NodeInfo() = default;

    NodeInfo(int r, const std::string& ip, uint16_t port)
    : rank(r), ip_addr(ip), data_port(port) 
    {}
};
