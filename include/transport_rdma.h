// transport_rdma.h
// Using RDMA to send/recv data base on libibverbs
//
// TODO: Dual QP Architecture for Hybrid Buffer Pool + Zero Copy
// ================================================================
// Current limitation: enable_zero_copy mode is mutually exclusive.
// When enabled, buffer pool is not used; when disabled, zero copy is not used.
//
// Proposed Enhancement: Use two QPs to support both modes simultaneously
//
// Architecture:
//   QP #1 (qp_small): For small data (< 1 MB threshold)
//     - Uses buffer pool with pre-registered MRs
//     - Pre-posts recv WRs for low latency
//     - Avoids per-transfer MR registration overhead
//
//   QP #2 (qp_large): For large data (>= 1 MB threshold)
//     - Uses zero copy with on-demand MR registration
//     - Posts recv WRs only when needed
//     - Avoids memory copy for large transfers
//
// Implementation Steps:
//   1. Add second QP to RdmaResources: ibv_qp* qp_small, qp_large
//   2. Create two CQs (or share one CQ with wr_id encoding QP type)
//   3. Modify InitRdmaResources() to create both QPs
//   4. Modify Connect()/Accept() to exchange two sets of QP info
//   5. Modify Send()/Recv() to select QP based on data size:
//      - size < threshold -> use qp_small (buffer pool)
//      - size >= threshold -> use qp_large (zero copy)
//   6. Buffer pool only posts recv WRs to qp_small
//   7. Add configurable threshold (default 1 MB)
//
// Benefits:
//   - Small messages: low latency (no MR registration)
//   - Large messages: high throughput (no memory copy)
//   - Automatic mode selection based on message size
// ================================================================

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <infiniband/verbs.h>
#include "transport.h"
#include "logger.h"

struct RdmaBuffer
{
    // Pointer to the buffer
    char* data = nullptr;
    size_t size = 0;
    // Memory region for this buffer
    struct ibv_mr* mr = nullptr;
    // Buffer ID for wr_id
    int id = -1;
    bool in_use = false;
};

struct RdmaResources
{
    // RDMA context and resources
    struct ibv_context* ctx = nullptr;
    struct ibv_pd* pd = nullptr;
    struct ibv_cq* cq = nullptr;
    struct ibv_qp* qp = nullptr;
    // Completion channel for event-driven polling
    struct ibv_comp_channel* comp_channel = nullptr;

    // Buffer pool (each buffer has its own MR and size)
    std::vector<RdmaBuffer> buffers;
    std::mutex buffer_mtx;

    // Local GID index
    int local_gid_index = 0;

    // Remote Info
    uint32_t remote_qpn = 0;
    uint16_t remote_lid = 0;
    // Zero-initialized
    union ibv_gid remote_gid = {};
    int remote_gid_index = 0;

    RdmaResources() = default;

    ~RdmaResources()
    {
        // Clean buffer pool
        for (auto& buf : buffers)
        {
            if (buf.mr)
            {
                ibv_dereg_mr(buf.mr);
                buf.mr = nullptr;
            }
            if (buf.data)
            {
                free(buf.data);
                buf.data = nullptr;
            }
        }

        // Clean RDMA resources for each connection
        if (qp)
        {
            ibv_destroy_qp(qp);
            qp = nullptr;
        }
        if (cq)
        {
            ibv_destroy_cq(cq);
            cq = nullptr;
        }
        if (comp_channel)
        {
            ibv_destroy_comp_channel(comp_channel);
            comp_channel = nullptr;
        }

        // pd and ctx are shared resources inside whole process,
        // will be released in RdmaSharedResources
    }
};

// QP info to be sent/recv
struct RdmaQpInfo
{
    uint32_t qpn = 0;
    uint16_t lid = 0;
    uint8_t gid[16] = {};
    int32_t gid_index = 0;
};

// RAII style memory region
class AutoMR
{
public:
    // Auto register
    AutoMR(struct ibv_pd* pd, void* addr, size_t length)
    {
        if (pd && addr && length > 0)
        {
            mr = ibv_reg_mr(pd, addr, length, 
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
            if (!mr)
            {
                LOG_ERROR("RDMA: Failed to register MR for address={}, length={}", addr, length);
            }
            else
            {
                LOG_DEBUG("RDMA: Register MR for address={}, length={}, lkey={}", addr, length, mr->lkey);
            }
        }
    }
    AutoMR()
    {
    }

    // Auto deregister
    ~AutoMR()
    {
        reset();
    }

    // No copy
    AutoMR(const AutoMR&) = delete;
    AutoMR& operator=(const AutoMR&) = delete;

    // Move
    AutoMR(AutoMR&& other) noexcept
    {
        mr = other.mr;
        other.mr = nullptr;
    }
    AutoMR& operator=(AutoMR&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            mr = other.mr;
            other.mr = nullptr;
        }

        return *this;
    }

    // Get MR pointer
    struct ibv_mr* get() const
    {
        return mr;
    }

    // Check if it is valid, used for if (mr)
    explicit operator bool() const
    {
        return mr != nullptr;
    }

    // Release ownership
    struct ibv_mr* release()
    {
        struct ibv_mr* tmp = mr;
        mr = nullptr;
        return tmp;
    }

    void reset()
    {
        if (!mr)
        {
            return;
        }

        if (ibv_dereg_mr(mr) != 0)
        {
            LOG_ERROR("RDMA: Failed to auto deregister MR");
        }
        else
        {
            LOG_DEBUG("RDMA: Auto deregister MR");
        }
        mr = nullptr;
    }

private:
    struct ibv_mr* mr = nullptr;
};

// All RDMA transport inside one process will share the context and PD
class RdmaSharedResources
{
public:
    // Singleton
    static RdmaSharedResources& GetInstance()
    {
        static RdmaSharedResources instance;
        return instance;
    }

    RdmaSharedResources(const RdmaSharedResources&) = delete;
    RdmaSharedResources& operator=(const RdmaSharedResources&) = delete;
    RdmaSharedResources(RdmaSharedResources&&) = delete;
    RdmaSharedResources& operator=(RdmaSharedResources&&) = delete;

    // Get shared RDMA context and pd
    bool Acquire(struct ibv_context** out_ctx, struct ibv_pd** out_pd)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (ref_count == 0)
        {
            // Init at the first call
            if (!Initialize())
            {
                return false;
            }
        }

        ref_count++;
        *out_ctx = ctx;
        *out_pd = pd;
        LOG_DEBUG("RDMA: Acquire shared resources, ref_count = {}", ref_count);
        return true;
    }

    // Release shared RDMA resources
    void Release()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (ref_count > 0)
        {
            ref_count--;
            LOG_DEBUG("RDMA: Release shared resources, ref_count = {}", ref_count);

            if (ref_count == 0)
            {
                Cleanup();
            }
        }
    }

private:
    RdmaSharedResources() = default;

    ~RdmaSharedResources()
    {
        Cleanup();
    }

    bool Initialize()
    {
        int num_devices = 0;
        struct ibv_device** device_list = ibv_get_device_list(&num_devices);
        if (!device_list || num_devices == 0)
        {
            LOG_ERROR("RDMA: No devices found");
            if (device_list)
            {
                ibv_free_device_list(device_list);
            }
            return false;
        }

        // TODO: Use the specific device instead of the first one
        LOG_INFO("RDMA: Found {} devices, using device {}", num_devices, ibv_get_device_name(device_list[0]));
        ctx = ibv_open_device(device_list[0]);
        ibv_free_device_list(device_list);
        if (!ctx)
        {
            LOG_ERROR("RDMA: Failed to open device");
            return false;
        }

        pd = ibv_alloc_pd(ctx);
        if (!pd)
        {
            LOG_ERROR("RDMA: Failed to allocate PD");
            ibv_close_device(ctx);
            ctx = nullptr;
            return false;
        }

        LOG_INFO("RDMA: Initialize RDMA shared resources (context and PD)");
        return true;
    }

    void Cleanup()
    {
        if (pd)
        {
            ibv_dealloc_pd(pd);
            pd = nullptr;
        }
        if (ctx)
        {
            ibv_close_device(ctx);
            ctx = nullptr;
        }
        if (ref_count == 0)
        {
            LOG_INFO("RDMA: Cleanup shared resources");
        }
    }

private:
    struct ibv_context* ctx = nullptr;
    struct ibv_pd* pd = nullptr;
    int ref_count = 0;
    std::mutex mtx;
};

class TransportRdma : public Transport
{
public:
    TransportRdma();
    ~TransportRdma() override;

    bool Connect(const std::string& addr, uint16_t port) override;
    bool Listen(uint16_t port) override;
    bool Accept() override;
    std::shared_ptr<Transport> CreateAcceptedConnection() override;
    bool Send(const void* data, size_t size) override;
    bool Recv(void* data, size_t size) override;
    void Close() override;
    
    uint16_t GetListenPort() const override
    {
        return listen_port;
    }
    bool IsConnected() const override
    {
        return connected;
    }

    // Performance tuning
    void SetBusyPolling(bool enabled)
    {
        enable_busy_polling = enabled;
    }
    void SetZeroCopy(bool enabled)
    {
        enable_zero_copy = enabled;
    }

    // Buffer pool configuration (must be called before Connect/Accept)
    void SetBufferPoolConfig(size_t buffer_size, int count)
    {
        buffer_size_config = buffer_size;
        buffer_count_config = count;
    }
    size_t GetBufferSize() const
    {
        return buffer_size_config;
    }
    int GetBufferCount() const
    {
        return buffer_count_config;
    }

    // Send/recv data with user buffer, by creating MR from the buffer
    bool SendZeroCopy(const void* data, size_t size);
    bool RecvZeroCopy(void* data, size_t size);

    // Communicator to get tcp_listen_fd to share among multi-accepts
    friend class Communicator;

private:
    bool InitRdmaResources();
    bool InitBufferPool();
    bool PostRecvWRsForBufferPool();

    bool ModifyQPToInit();
    bool ModifyQPToRTR();
    bool ModifyQPToRTS();
    bool ExchangeQPInfo(int tcp_sock, bool is_initiator);

    bool PostSendWR(void* buffer_addr, size_t size, uint32_t lkey, bool signaled = true);
    // Return buffer ID if available
    bool PollCompletion(int* out_buffer_id = nullptr);

    // Manage Buffer Pool
    // Allocate a free buffer, return its buffer ID
    int AllocateBuffer();
    void ReleaseBuffer(int buffer_id);

    // Zero Copy
    // Do not acquire lock, caller should protect it
    bool SendZeroCopyInternal(const void* data, size_t size);
    bool RecvZeroCopyInternal(void* data, size_t size);

    // Do not acquire lock, caller should protect it
    bool SendBufferPoolInternal(const void* data, size_t size);
    bool RecvBufferPoolInternal(void* data, size_t size);

    // Get best GID index for IB, RoCE, SoftRoCE
    int FindBestGidIndex(union ibv_gid* out_gid);

    void EncodeQpInfo(const RdmaQpInfo& qp_info, std::vector<char>& result);
    RdmaQpInfo DecodeQpInfo(const char* data, size_t& offset);

private:
    // Constants
    static constexpr size_t DEFAULT_BUFFER_SIZE = 16 * 1024 * 1024;  // 16 MB
    static constexpr int DEFAULT_BUFFER_COUNT = 16;
    static constexpr size_t INLINE_THRESHOLD = 256;

    // RDMA resources
    std::unique_ptr<RdmaResources> rdma_res;

    // TCP socket for QP info exchange
    int tcp_sock_fd = -1;
    int tcp_listen_fd = -1;
    uint16_t listen_port = 0;
    bool connected = false;
    bool shared_resources_acquired = false;

    // Protect send/recv
    std::mutex send_mtx;
    std::mutex recv_mtx;

    // Performance tuning
    bool enable_busy_polling = true;
    // Default: use buffer pool mode
    bool enable_zero_copy = false;

    // Buffer pool params
    size_t buffer_size_config = DEFAULT_BUFFER_SIZE;
    int buffer_count_config = DEFAULT_BUFFER_COUNT;
};
