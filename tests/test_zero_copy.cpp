/*
Test RDMA Zero Copy vs Buffer Pool performance

Usage:
  Terminal 1 (server): ./test_zero_copy server
  Terminal 2 (client): ./test_zero_copy client

The test will run both modes and compare results.
*/

#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "transport_rdma.h"
#include "logger.h"

constexpr uint16_t PORT_ZEROCOPY = 23456;
constexpr uint16_t PORT_BUFFERPOOL = 23457;

// Test sizes
struct TestSize {
    size_t size;
    const char* name;
};

const TestSize TEST_SIZES[] = {
    {1024,               "1 KB"},
    {4 * 1024,           "4 KB"},
    {16 * 1024,          "16 KB"},
    {64 * 1024,          "64 KB"},
    {256 * 1024,         "256 KB"},
    {1024 * 1024,        "1 MB"},
    {4 * 1024 * 1024,    "4 MB"},
    {16 * 1024 * 1024,   "16 MB"},
    {64 * 1024 * 1024,   "64 MB"},
    {128 * 1024 * 1024,  "128 MB"},
    {256 * 1024 * 1024,  "256 MB"},
};
constexpr int NUM_SIZES = sizeof(TEST_SIZES) / sizeof(TEST_SIZES[0]);

struct TestResult {
    size_t size;
    int64_t time_us;
    double throughput_mbps;
};

void FillBuffer(char* buf, size_t size, char pattern)
{
    for (size_t i = 0; i < size; ++i)
    {
        buf[i] = pattern + (i % 26);
    }
}

bool VerifyBuffer(const char* buf, size_t size, char pattern)
{
    for (size_t i = 0; i < size; ++i)
    {
        if (buf[i] != static_cast<char>(pattern + (i % 26)))
        {
            LOG_ERROR("Data mismatch at offset {}", i);
            return false;
        }
    }
    return true;
}

bool RunServerTest(uint16_t port, bool zero_copy, std::vector<TestResult>& results)
{
    const char* mode_str = zero_copy ? "Zero Copy" : "Buffer Pool";
    LOG_INFO("--- Server: Testing {} mode on port {} ---", mode_str, port);
    
    TransportRdma transport;
    transport.SetZeroCopy(zero_copy);
    transport.SetBusyPolling(true);
    
    if (!transport.Listen(port))
    {
        LOG_ERROR("Server: Failed to listen on port {}", port);
        return false;
    }
    
    if (!transport.Accept())
    {
        LOG_ERROR("Server: Failed to accept");
        return false;
    }
    LOG_INFO("Server: Client connected ({})", mode_str);
    
    results.clear();
    for (int i = 0; i < NUM_SIZES; ++i)
    {
        size_t size = TEST_SIZES[i].size;
        char pattern = 'A' + i;
        
        std::vector<char> recv_buf(size);
        
        auto start = std::chrono::high_resolution_clock::now();
        if (!transport.Recv(recv_buf.data(), size))
        {
            LOG_ERROR("Server: Failed to receive {}", TEST_SIZES[i].name);
            return false;
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        if (!VerifyBuffer(recv_buf.data(), size, pattern))
        {
            LOG_ERROR("Server: Data verification failed for {}", TEST_SIZES[i].name);
            return false;
        }
        
        TestResult result;
        result.size = size;
        result.time_us = duration;
        result.throughput_mbps = (size / 1024.0 / 1024.0) / (duration / 1000000.0);
        results.push_back(result);
    }
    
    transport.Close();
    return true;
}

bool RunClientTest(uint16_t port, bool zero_copy, std::vector<TestResult>& results)
{
    const char* mode_str = zero_copy ? "Zero Copy" : "Buffer Pool";
    LOG_INFO("--- Client: Testing {} mode on port {} ---", mode_str, port);
    
    TransportRdma transport;
    transport.SetZeroCopy(zero_copy);
    transport.SetBusyPolling(true);
    
    // Retry connection with wait (server may not be ready yet)
    bool connected = false;
    for (int retry = 0; retry < 10; ++retry)
    {
        if (transport.Connect("127.0.0.1", port))
        {
            connected = true;
            break;
        }
        LOG_INFO("Client: Waiting for server on port {}... (retry {})", port, retry + 1);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    if (!connected)
    {
        LOG_ERROR("Client: Failed to connect to port {} after retries", port);
        return false;
    }
    LOG_INFO("Client: Connected ({})", mode_str);
    
    results.clear();
    for (int i = 0; i < NUM_SIZES; ++i)
    {
        size_t size = TEST_SIZES[i].size;
        char pattern = 'A' + i;
        
        std::vector<char> send_buf(size);
        FillBuffer(send_buf.data(), size, pattern);
        
        auto start = std::chrono::high_resolution_clock::now();
        if (!transport.Send(send_buf.data(), size))
        {
            LOG_ERROR("Client: Failed to send {}", TEST_SIZES[i].name);
            return false;
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        TestResult result;
        result.size = size;
        result.time_us = duration;
        result.throughput_mbps = (size / 1024.0 / 1024.0) / (duration / 1000000.0);
        results.push_back(result);
    }
    
    transport.Close();
    return true;
}

void PrintComparison(const std::vector<TestResult>& zc_results, 
                     const std::vector<TestResult>& bp_results)
{
    LOG_INFO("");
    LOG_INFO("================================================================================");
    LOG_INFO("                      PERFORMANCE COMPARISON RESULTS                           ");
    LOG_INFO("================================================================================");
    LOG_INFO("{:>10} | {:>12} {:>12} | {:>12} {:>12} | {:>8}",
             "Size", "ZC Time(us)", "ZC MB/s", "BP Time(us)", "BP MB/s", "Speedup");
    LOG_INFO("{}", std::string(82, '-'));
    
    for (int i = 0; i < NUM_SIZES; ++i)
    {
        double speedup = static_cast<double>(bp_results[i].time_us) / zc_results[i].time_us;
        const char* winner = (speedup > 1.0) ? "ZC" : "BP";
        
        LOG_INFO("{:>10} | {:>12} {:>12.2f} | {:>12} {:>12.2f} | {:>6.2f}x {}",
                 TEST_SIZES[i].name,
                 zc_results[i].time_us, zc_results[i].throughput_mbps,
                 bp_results[i].time_us, bp_results[i].throughput_mbps,
                 speedup > 1.0 ? speedup : 1.0/speedup,
                 winner);
    }
    LOG_INFO("{}", std::string(82, '-'));
    LOG_INFO("ZC = Zero Copy, BP = Buffer Pool");
    LOG_INFO("Speedup > 1 means Zero Copy is faster");
    LOG_INFO("");
}

void RunServer()
{
    LOG_INFO("=== RDMA Performance Test - Server ===");
    LOG_INFO("");
    
    std::vector<TestResult> zc_results, bp_results;
    
    // Test 1: Zero Copy
    if (!RunServerTest(PORT_ZEROCOPY, true, zc_results))
    {
        LOG_ERROR("Zero Copy test failed");
        return;
    }
    LOG_INFO("Server: Zero Copy test completed");
    
    // Brief pause between tests
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Test 2: Buffer Pool
    if (!RunServerTest(PORT_BUFFERPOOL, false, bp_results))
    {
        LOG_ERROR("Buffer Pool test failed");
        return;
    }
    LOG_INFO("Server: Buffer Pool test completed");
    
    // Print comparison
    PrintComparison(zc_results, bp_results);
    
    LOG_INFO("=== Server: All tests passed! ===");
}

void RunClient()
{
    LOG_INFO("=== RDMA Performance Test - Client ===");
    LOG_INFO("");
    
    std::vector<TestResult> zc_results, bp_results;
    
    // Test 1: Zero Copy
    if (!RunClientTest(PORT_ZEROCOPY, true, zc_results))
    {
        LOG_ERROR("Zero Copy test failed");
        return;
    }
    LOG_INFO("Client: Zero Copy test completed");
    
    // Brief pause between tests
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Test 2: Buffer Pool
    if (!RunClientTest(PORT_BUFFERPOOL, false, bp_results))
    {
        LOG_ERROR("Buffer Pool test failed");
        return;
    }
    LOG_INFO("Client: Buffer Pool test completed");
    
    // Print comparison
    PrintComparison(zc_results, bp_results);
    
    LOG_INFO("=== Client: All tests completed! ===");
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " <server|client>" << std::endl;
        std::cout << std::endl;
        std::cout << "This test compares Zero Copy vs Buffer Pool performance." << std::endl;
        std::cout << "Run server first, then client in another terminal." << std::endl;
        return 1;
    }
    
    Logger::Instance().SetLogLevel(LogLevel::INFO);
    
    std::string role = argv[1];
    if (role == "server")
    {
        RunServer();
    }
    else if (role == "client")
    {
        RunClient();
    }
    else
    {
        std::cout << "Unknown role: " << role << std::endl;
        std::cout << "Usage: " << argv[0] << " <server|client>" << std::endl;
        return 1;
    }
    
    return 0;
}
