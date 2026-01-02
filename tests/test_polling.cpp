// test_polling.cpp
// Compare busy polling vs event-driven polling performance

#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <sys/resource.h>  // for getrusage
#include "transport_rdma.h"
#include "logger.h"

struct LatencyResult
{
    double min_us;
    double max_us;
    double avg_us;
    double p50_us;
    double p99_us;
    double cpu_percent;  // CPU usage percentage
};

// Get CPU time in microseconds (user + system)
double GetCpuTimeUs()
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double user_us = usage.ru_utime.tv_sec * 1e6 + usage.ru_utime.tv_usec;
    double sys_us = usage.ru_stime.tv_sec * 1e6 + usage.ru_stime.tv_usec;
    return user_us + sys_us;
}

LatencyResult CalculateLatency(std::vector<double>& latencies, double cpu_percent)
{
    std::sort(latencies.begin(), latencies.end());
    
    LatencyResult result;
    result.min_us = latencies.front();
    result.max_us = latencies.back();
    result.avg_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    result.p50_us = latencies[latencies.size() / 2];
    result.p99_us = latencies[latencies.size() * 99 / 100];
    result.cpu_percent = cpu_percent;
    
    return result;
}

bool RunServerTest(uint16_t port, bool busy_polling)
{
    const char* mode_str = busy_polling ? "Busy Polling" : "Event-Driven";
    LOG_INFO("--- Server: Testing {} mode on port {} ---", mode_str, port);
    
    TransportRdma transport;
    transport.SetZeroCopy(false);  // Use buffer pool
    transport.SetBusyPolling(busy_polling);
    
    if (!transport.Listen(port))
    {
        LOG_ERROR("Server: Failed to listen on port {}", port);
        return false;
    }
    
    LOG_INFO("Server: Waiting for connection...");
    if (!transport.Accept())
    {
        LOG_ERROR("Server: Failed to accept connection");
        return false;
    }
    LOG_INFO("Server: Client connected ({})", mode_str);
    
    // Test sizes: 64B, 256B, 1KB, 4KB, 16KB, 64KB
    std::vector<size_t> test_sizes = {64, 256, 1024, 4096, 16384, 65536};
    const int iterations = 1000;
    const int warmup = 100;
    
    for (size_t size : test_sizes)
    {
        std::vector<char> buffer(size);
        
        // Warmup + actual iterations
        for (int i = 0; i < warmup + iterations; i++)
        {
            // Receive ping
            if (!transport.Recv(buffer.data(), size))
            {
                LOG_ERROR("Server: Failed to receive at iteration {}", i);
                return false;
            }
            
            // Send pong
            if (!transport.Send(buffer.data(), size))
            {
                LOG_ERROR("Server: Failed to send at iteration {}", i);
                return false;
            }
        }
        
        LOG_INFO("Server: Completed {} bytes test", size);
    }
    
    // Wait for done signal
    int done = 0;
    transport.Recv(&done, sizeof(done));
    
    LOG_INFO("Server: {} test completed", mode_str);
    return true;
}

bool RunClientTest(uint16_t port, bool busy_polling, 
                   std::vector<std::pair<size_t, LatencyResult>>& results)
{
    const char* mode_str = busy_polling ? "Busy Polling" : "Event-Driven";
    LOG_INFO("--- Client: Testing {} mode on port {} ---", mode_str, port);
    
    TransportRdma transport;
    transport.SetZeroCopy(false);  // Use buffer pool
    transport.SetBusyPolling(busy_polling);
    
    // Retry connection
    int max_retries = 10;
    bool connected = false;
    for (int retry = 0; retry < max_retries && !connected; retry++)
    {
        if (transport.Connect("127.0.0.1", port))
        {
            connected = true;
        }
        else
        {
            LOG_INFO("Client: Waiting for server... (retry {})", retry + 1);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    if (!connected)
    {
        LOG_ERROR("Client: Failed to connect");
        return false;
    }
    LOG_INFO("Client: Connected ({})", mode_str);
    
    // Test sizes: 64B, 256B, 1KB, 4KB, 16KB, 64KB
    std::vector<size_t> test_sizes = {64, 256, 1024, 4096, 16384, 65536};
    const int iterations = 1000;
    const int warmup = 100;
    
    for (size_t size : test_sizes)
    {
        std::vector<char> send_buffer(size, 'A');
        std::vector<char> recv_buffer(size);
        std::vector<double> latencies;
        latencies.reserve(iterations);
        
        // Warmup
        for (int i = 0; i < warmup; i++)
        {
            if (!transport.Send(send_buffer.data(), size)) return false;
            if (!transport.Recv(recv_buffer.data(), size)) return false;
        }
        
        // Measure CPU time and wall time during actual iterations
        double cpu_start = GetCpuTimeUs();
        auto wall_start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < iterations; i++)
        {
            auto start = std::chrono::high_resolution_clock::now();
            
            // Send ping
            if (!transport.Send(send_buffer.data(), size))
            {
                LOG_ERROR("Client: Failed to send at iteration {}", i);
                return false;
            }
            
            // Receive pong
            if (!transport.Recv(recv_buffer.data(), size))
            {
                LOG_ERROR("Client: Failed to receive at iteration {}", i);
                return false;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            double latency_us = std::chrono::duration<double, std::micro>(end - start).count();
            latencies.push_back(latency_us);
        }
        
        auto wall_end = std::chrono::high_resolution_clock::now();
        double cpu_end = GetCpuTimeUs();
        
        // Calculate CPU usage percentage
        double wall_time_us = std::chrono::duration<double, std::micro>(wall_end - wall_start).count();
        double cpu_time_us = cpu_end - cpu_start;
        double cpu_percent = (cpu_time_us / wall_time_us) * 100.0;
        
        // Calculate statistics
        LatencyResult result = CalculateLatency(latencies, cpu_percent);
        results.push_back({size, result});
        
        LOG_INFO("  {} bytes: avg={:.1f}us, p99={:.1f}us, CPU={:.1f}%",
                 size, result.avg_us, result.p99_us, result.cpu_percent);
    }
    
    // Send done signal
    int done = 1;
    transport.Send(&done, sizeof(done));
    
    LOG_INFO("Client: {} test completed", mode_str);
    return true;
}

void PrintComparison(const std::vector<std::pair<size_t, LatencyResult>>& busy_results,
                     const std::vector<std::pair<size_t, LatencyResult>>& event_results)
{
    LOG_INFO("");
    LOG_INFO("=== Performance Comparison: Busy Polling vs Event-Driven ===");
    LOG_INFO("");
    LOG_INFO("{:>8} | {:>10} {:>8} | {:>10} {:>8} | {:>8} {:>10}",
             "Size", "Busy Avg", "CPU%", "Event Avg", "CPU%", "Speedup", "CPU Save");
    LOG_INFO("{:-^8}-+-{:-^10}-{:-^8}-+-{:-^10}-{:-^8}-+-{:-^8}-{:-^10}",
             "", "", "", "", "", "", "");
    
    for (size_t i = 0; i < busy_results.size(); i++)
    {
        size_t size = busy_results[i].first;
        const auto& busy = busy_results[i].second;
        const auto& event = event_results[i].second;
        
        double speedup = event.avg_us / busy.avg_us;
        double cpu_save = busy.cpu_percent - event.cpu_percent;
        
        std::string size_str;
        if (size >= 1024)
            size_str = std::to_string(size / 1024) + " KB";
        else
            size_str = std::to_string(size) + " B";
        
        LOG_INFO("{:>8} | {:>8.1f}us {:>7.1f}% | {:>8.1f}us {:>7.1f}% | {:>7.2f}x {:>9.1f}%",
                 size_str, busy.avg_us, busy.cpu_percent, 
                 event.avg_us, event.cpu_percent, speedup, cpu_save);
    }
    
    LOG_INFO("");
    LOG_INFO("Speedup > 1.0 = Busy Polling faster | CPU Save > 0 = Event-Driven saves CPU");
}

int main(int argc, char* argv[])
{
    Logger::Instance().SetLogLevel(LogLevel::INFO);
    
    if (argc < 2)
    {
        fmt::print("Usage: {} <server|client>\n", argv[0]);
        fmt::print("\nThis test compares RDMA busy polling vs event-driven polling latency.\n");
        fmt::print("Uses ping-pong pattern to measure round-trip time.\n");
        fmt::print("\nRun in two terminals:\n");
        fmt::print("  Terminal 1: {} server\n", argv[0]);
        fmt::print("  Terminal 2: {} client\n", argv[0]);
        return 1;
    }
    
    std::string role = argv[1];
    const uint16_t busy_port = 23460;
    const uint16_t event_port = 23461;
    
    if (role == "server")
    {
        LOG_INFO("=== RDMA Polling Mode Test - Server ===");
        
        // Test 1: Busy polling
        if (!RunServerTest(busy_port, true))
        {
            LOG_ERROR("Busy polling test failed");
            return 1;
        }
        
        // Test 2: Event-driven polling
        if (!RunServerTest(event_port, false))
        {
            LOG_ERROR("Event-driven test failed");
            return 1;
        }
        
        LOG_INFO("=== Server: All tests completed ===");
    }
    else if (role == "client")
    {
        LOG_INFO("=== RDMA Polling Mode Test - Client ===");
        
        std::vector<std::pair<size_t, LatencyResult>> busy_results;
        std::vector<std::pair<size_t, LatencyResult>> event_results;
        
        // Test 1: Busy polling
        if (!RunClientTest(busy_port, true, busy_results))
        {
            LOG_ERROR("Busy polling test failed");
            return 1;
        }
        
        // Short delay before next test
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Test 2: Event-driven polling
        if (!RunClientTest(event_port, false, event_results))
        {
            LOG_ERROR("Event-driven test failed");
            return 1;
        }
        
        // Print comparison
        PrintComparison(busy_results, event_results);
        
        LOG_INFO("=== Client: All tests completed ===");
    }
    else
    {
        fmt::print("Unknown role: {}. Use 'server' or 'client'.\n", role);
        return 1;
    }
    
    return 0;
}

