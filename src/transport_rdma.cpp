// transport_rdma.cpp
// Using RDMA to send/recv data
// 

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "transport_rdma.h"
#include "logger.h"
#include "utils.h"

TransportRdma::TransportRdma() = default;

TransportRdma::~TransportRdma()
{
    Close();
}

bool TransportRdma::Connect(const std::string& addr, uint16_t port)
{
    if (!InitRdmaResources())
    {
        LOG_ERROR("RDMA: Failed to init RDMA context when connect");
        return false;
    }

    // Create socket to exchange QP info
    tcp_sock_fd = Utils::CreateConnectSocket(addr, port, 1);
    if (tcp_sock_fd < 0)
    {
        Close();
        LOG_ERROR("RDMA: Failed to create socket when connect");
        return false;
    }

    if (!ModifyQPToInit())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to Init when connect");
        return false;
    }

    if (!ExchangeQPInfo(tcp_sock_fd, true))
    {
        Close();
        LOG_ERROR("RDMA: Failed to exchange QP info when connect");
        return false;
    }

    if (!ModifyQPToRTR())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to RTR when connect");
        return false;
    }

    if (!ModifyQPToRTS())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to RTS when connect");
        return false;
    }

    // Post recv WRs for all buffers (only if zero copy is disabled)
    // When zero copy is enabled, recv WRs are posted on-demand
    if (!enable_zero_copy)
    {
        if (!PostRecvWRsForBufferPool())
        {
            Close();
            return false;
        }
    }

    connected = true;
    LOG_INFO("RDMA: Connection established to {}:{}", addr, port);
    return true;
}

bool TransportRdma::Listen(uint16_t port)
{
    // Create TCP socket
    tcp_listen_fd = Utils::CreateListenSocket(port, &listen_port);
    if (tcp_listen_fd < 0)
    {
        LOG_ERROR("RDMA: Failed to create TCP listen socket at port {}", port);
        return false;
    }

    LOG_INFO("RDMA: Create TCP listen socket at port {}", listen_port);
    return true;
}

bool TransportRdma::Accept()
{
    if (tcp_listen_fd < 0)
    {
        LOG_ERROR("RDMA: Not in listen mode");
        return false;
    }

    if (!InitRdmaResources())
    {
        LOG_ERROR("RDMA: Failed to init RDMA context when accept");
        return false;
    }

    tcp_sock_fd = Utils::AcceptConnection(tcp_listen_fd);
    if (tcp_sock_fd < 0)
    {
        Close();
        LOG_ERROR("RDMA: Failed to accept connection when accept");
        return false;
    }
    LOG_INFO("RDMA: TCP connection accepted");

    if (!ModifyQPToInit())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to Init when accept");
        return false;
    }

    if (!ExchangeQPInfo(tcp_sock_fd, false))
    {
        Close();
        LOG_ERROR("RDMA: Failed to exchange QP info when accept");
        return false;
    }

    if (!ModifyQPToRTR())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to RTR when accept");
        return false;
    }

    if (!ModifyQPToRTS())
    {
        Close();
        LOG_ERROR("RDMA: Failed to modify QP to RTS when accept");
        return false;
    }

    // Post recv WRs for all buffers (only if zero copy is disabled)
    // When zero copy is enabled, recv WRs are posted on-demand
    if (!enable_zero_copy)
    {
        if (!PostRecvWRsForBufferPool())
        {
            Close();
            return false;
        }
    }

    connected = true;
    LOG_INFO("RDMA: Connection accepted and established");
    return true;
}

std::shared_ptr<Transport> TransportRdma::CreateAcceptedConnection()
{
    if (tcp_listen_fd < 0)
    {
        LOG_ERROR("RDMA: Not in listen mode");
        return nullptr;
    }

    // Create new instance
    auto new_transport = std::make_shared<TransportRdma>();
    new_transport->tcp_listen_fd = tcp_listen_fd;

    // Create RDMA connection
    if (!new_transport->Accept())
    {
        LOG_ERROR("RDMA: Failed to accept new connection");
        return nullptr;
    }

    LOG_DEBUG("RDMA: Accept new connection");
    return new_transport;
}

bool TransportRdma::Send(const void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(send_mtx);

    if (!connected)
    {
        LOG_ERROR("RDMA: Not connected for send");
        return false;
    }

    // Mode selection:
    // - Zero copy mode: register user buffer directly (all sizes)
    // - Buffer pool mode: copy data to pre-registered buffers
    if (enable_zero_copy)
    {
        // Zero copy: all sizes use direct MR registration
        LOG_DEBUG("RDMA: Send {} bytes using zero copy", size);
        return SendZeroCopyInternal(data, size);
    }
    else
    {
        // Buffer pool: send in chunks
        // Each chunk needs space for size header (8 bytes)
        size_t max_payload = buffer_size_config - sizeof(uint64_t);
        
        if (size <= max_payload)
        {
            LOG_DEBUG("RDMA: Send {} bytes in single chunk", size);
            return SendBufferPoolInternal(data, size);
        }
        
        // Large data: multiple chunks
        LOG_DEBUG("RDMA: Send {} bytes in multiple chunks", size);
        size_t offset = 0;
        const char* data_ptr = static_cast<const char*>(data);
        while (offset < size)
        {
            size_t chunk_size = std::min(size - offset, max_payload);
            if (!SendBufferPoolInternal(data_ptr + offset, chunk_size))
            {
                LOG_ERROR("RDMA: Failed to send chunk at offset {}", offset);
                return false;
            }
            
            offset += chunk_size;
        }

        LOG_DEBUG("RDMA: Complete send {} bytes in {} chunks", size, (size + max_payload - 1) / max_payload);
        return true;
    }
}

bool TransportRdma::Recv(void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(recv_mtx);

    if (!connected)
    {
        LOG_ERROR("RDMA: Not connected for recv");
        return false;
    }

    // Mode selection:
    // - Zero copy mode: register user buffer directly (all sizes)
    // - Buffer pool mode: copy data from pre-registered buffers
    if (enable_zero_copy)
    {
        // Zero copy: all sizes use direct MR registration
        LOG_DEBUG("RDMA: Recv {} bytes using zero copy", size);
        return RecvZeroCopyInternal(data, size);
    }
    else
    {
        // Buffer pool: recv in chunks
        // Each chunk needs space for size header (8 bytes)
        size_t max_payload = buffer_size_config - sizeof(uint64_t);
        
        if (size <= max_payload)
        {
            LOG_DEBUG("RDMA: Recv {} bytes in single chunk", size);
            return RecvBufferPoolInternal(data, size);
        }
        
        // Large data: multiple chunks
        LOG_DEBUG("RDMA: Recv {} bytes in multiple chunks", size);
        size_t offset = 0;
        char* data_ptr = static_cast<char*>(data);
        while (offset < size)
        {
            size_t chunk_size = std::min(size - offset, max_payload);
            if (!RecvBufferPoolInternal(data_ptr + offset, chunk_size))
            {
                LOG_ERROR("RDMA: Failed to recv chunk at offset {}", offset);
                return false;
            }
            
            offset += chunk_size;
        }

        LOG_DEBUG("RDMA: Complete recv {} bytes in {} chunks", size, (size + max_payload - 1) / max_payload);
        return true;
    }
}

void TransportRdma::Close()
{
    if (tcp_sock_fd >= 0)
    {
        close(tcp_sock_fd);
        tcp_sock_fd = -1;
    }
    if (tcp_listen_fd >= 0)
    {
        close(tcp_listen_fd);
        tcp_listen_fd = -1;
    }

    if (rdma_res)
    {
        // Relase QP, CQ, MR...
        rdma_res.reset();
    }

    // Release shared resources
    if (shared_resources_acquired)
    {
        RdmaSharedResources::GetInstance().Release();
        shared_resources_acquired = false;
    }

    connected = false;
}

// Testing for zero copy
bool TransportRdma::SendZeroCopy(const void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(send_mtx);
    if (!connected)
    {
        LOG_ERROR("RDMA: Not connected");
        return false;
    }

    return SendZeroCopyInternal(data, size);
}

bool TransportRdma::RecvZeroCopy(void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(recv_mtx);
    if (!connected)
    {
        LOG_ERROR("RDMA: Not connected");
        return false;
    }

    return RecvZeroCopyInternal(data, size);
}

bool TransportRdma::InitRdmaResources()
{
    bool success = false;
    rdma_res = std::make_unique<RdmaResources>();

    do
    {
        // Get shared context and PD
        if (!RdmaSharedResources::GetInstance().Acquire(&rdma_res->ctx, &rdma_res->pd))
        {
            rdma_res.reset();
            return false;
        }
        shared_resources_acquired = true;

        // Query port
        struct ibv_port_attr port_attr;
        if (ibv_query_port(rdma_res->ctx, 1, &port_attr) != 0)
        {
            LOG_ERROR("RDMA: Failed to query port");
            break;
        }
        LOG_DEBUG("RDMA: Port state {}, LID {}", port_attr.state, port_attr.lid);

        // PD has been acquired from RdmaSharedResources
        // Create completion channel for event-driven polling
        rdma_res->comp_channel = ibv_create_comp_channel(rdma_res->ctx);
        if (!rdma_res->comp_channel)
        {
            LOG_ERROR("RDMA: Failed to create completion channel");
            break;
        }

        // Create CQ with completion channel for each connection
        rdma_res->cq = ibv_create_cq(rdma_res->ctx, 128, nullptr, rdma_res->comp_channel, 0);
        if (!rdma_res->cq)
        {
            LOG_ERROR("RDMA: Failed to create CQ");
            break;
        }

        // Create QP
        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.send_cq = rdma_res->cq;
        qp_init_attr.recv_cq = rdma_res->cq;
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.cap.max_send_wr = 16;
        qp_init_attr.cap.max_recv_wr = 16;
        // Need 2 SGEs for zero copy: size header + data
        qp_init_attr.cap.max_send_sge = 2;
        qp_init_attr.cap.max_recv_sge = 2;
        rdma_res->qp = ibv_create_qp(rdma_res->pd, &qp_init_attr);
        if (!rdma_res->qp)
        {
            LOG_ERROR("RDMA: Failed to create QP");
            break;
        }
        LOG_DEBUG("RDMA: QP created with QPN {}", rdma_res->qp->qp_num);

        // Init buffer pool (each buffer has its own MR)
        if (!InitBufferPool())
        {
            LOG_ERROR("RDMA: Failed to init buffer pool");
            break;
        }

        success = true;
    } while (0);

    if (!success)
    {
        rdma_res.reset();
        if (shared_resources_acquired)
        {
            RdmaSharedResources::GetInstance().Release();
            shared_resources_acquired = false;
        }
    }
    
    return success;
}

bool TransportRdma::InitBufferPool()
{
    if (!rdma_res)
    {
        LOG_ERROR("RDMA: Failed to init buffer pool - rdma_res is null");
        return false;
    }

    // Init independent buffers
    rdma_res->buffers.resize(buffer_count_config);
    for (int i = 0; i < buffer_count_config; ++i)
    {
        RdmaBuffer& buf = rdma_res->buffers[i];
        buf.id = i;
        buf.size = buffer_size_config;
        buf.in_use = false;
        buf.data = nullptr;
        buf.mr = nullptr;

        // Allocate buffer
        buf.data = static_cast<char*>(malloc(buf.size));
        if (!buf.data)
        {
            LOG_ERROR("RDMA: Failed to allocate buffer {}", i);
            // Clean allocated buffers and return
            for (int j = 0; j < i; ++j)
            {
                if (rdma_res->buffers[j].mr)
                {
                    ibv_dereg_mr(rdma_res->buffers[j].mr);
                }
                if (rdma_res->buffers[j].data)
                {
                    free(rdma_res->buffers[j].data);
                }
            }
            return false;
        }

        // Regeister MR
        buf.mr = ibv_reg_mr(rdma_res->pd, buf.data, buf.size, 
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        if (!buf.mr)
        {
            LOG_ERROR("RDMA: Failed to register MR for buffer {}", i);
            free(buf.data);
            buf.data = nullptr;
            
            // Clean allocated buffers and return
            for (int j = 0; j < i; ++j)
            {
                if (rdma_res->buffers[j].mr)
                {
                    ibv_dereg_mr(rdma_res->buffers[j].mr);
                }
                if (rdma_res->buffers[j].data)
                {
                    free(rdma_res->buffers[j].data);
                }
            }
            return false;
        }
    }

    LOG_INFO("RDMA: Init buffer pool with {} buffers, each buffer is {} bytes", buffer_count_config, buffer_size_config);
    return true;
}

bool TransportRdma::PostRecvWRsForBufferPool()
{
    for (size_t i = 0; i < rdma_res->buffers.size(); ++i)
    {
        auto& buf = rdma_res->buffers[i];
        struct ibv_sge sge;
        sge.addr = reinterpret_cast<uint64_t>(buf.data);
        sge.length = buf.size;
        sge.lkey = buf.mr->lkey;

        struct ibv_recv_wr wr;
        memset(&wr, 0, sizeof(wr));
        // 2 for recv, lower 32b for buffer_id
        wr.wr_id = (2ULL << 32) | buf.id;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        struct ibv_recv_wr* bad_wr = nullptr;
        if (ibv_post_recv(rdma_res->qp, &wr, &bad_wr) != 0)
        {
            LOG_ERROR("RDMA: Failed to post recv WR for buffer {}", i);
            return false;
        }
        LOG_DEBUG("RDMA: Post recv WR for buffer {}", i);
    }
    return true;
}

bool TransportRdma::ModifyQPToInit()
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX |IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(rdma_res->qp, &attr, flags) != 0)
    {
        LOG_ERROR("RDMA: Failed to modify QP to INIT state");
        return false;
    }

    LOG_INFO("RDMA: Modify QP to INIT state");
    return true;
}

bool TransportRdma::ModifyQPToRTR()
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    // Use 4K for RoCE
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = rdma_res->remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;

    attr.ah_attr.dlid = rdma_res->remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    struct ibv_port_attr port_attr;
    if (ibv_query_port(rdma_res->ctx, 1, &port_attr) != 0)
    {
        LOG_ERROR("RDMA: Failed to query port for RTR");
        return false;
    }

    // Using RoCE or IB
    if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
    {
        // RoCE
        LOG_INFO("RDMA: Using RoCE with local GID index {}", rdma_res->local_gid_index);
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = rdma_res->remote_gid;
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.sgid_index = rdma_res->local_gid_index;
        attr.ah_attr.grh.hop_limit = 255;
        attr.ah_attr.grh.traffic_class = 0;
    }
    else
    {
        // InfiniBand
        LOG_INFO("RDMA: Using InfiniBand");
        attr.ah_attr.is_global = 0;
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(rdma_res->qp, &attr, flags) != 0)
    {
        LOG_ERROR("RDMA: Failed to modify QP to RTR state");
        return false;
    }

    LOG_INFO("RDMA: Modify QP to RTR state");
    return true;
}

bool TransportRdma::ModifyQPToRTS()
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;

    int flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(rdma_res->qp, &attr, flags) != 0)
    {
        LOG_ERROR("RDMA: Failed to modify QP to RTS state");
        return false;
    }

    LOG_INFO("RDMA: Modify QP to RTS state");
    return true;
}

bool TransportRdma::ExchangeQPInfo(int tcp_sock, bool is_initiator)
{
    // Prepare local QP
    struct ibv_port_attr port_attr;
    if (ibv_query_port(rdma_res->ctx, 1, &port_attr) != 0)
    {
        LOG_ERROR("RDMA: Failed to query port");
        return false;
    }

    // Find best GID to fit IB/RoCE/Soft RoCE
    union ibv_gid local_gid;
    rdma_res->local_gid_index = FindBestGidIndex(&local_gid);

    // Build local QP info
    RdmaQpInfo local_qp_info;
    local_qp_info.qpn = rdma_res->qp->qp_num;
    local_qp_info.lid = port_attr.lid;
    memcpy(local_qp_info.gid, local_gid.raw, 16);
    local_qp_info.gid_index = rdma_res->local_gid_index;

    // Encode it
    std::vector<char> send_buffer;
    EncodeQpInfo(local_qp_info, send_buffer);

    std::vector<char> recv_buffer;
    // Send first or recv first to avoid deadlock
    if (is_initiator)
    {
        // Send first, then recv
        if (!Utils::SendAll(tcp_sock, send_buffer))
        {
            LOG_ERROR("RDMA: Failed to send QP Info");
            return false;
        }
        if (!Utils::RecvAll(tcp_sock, recv_buffer))
        {
            LOG_ERROR("RDMA: Failed to recv QP Info");
            return false;
        }
    }
    else
    {
        // Recv first, then send
        if (!Utils::RecvAll(tcp_sock, recv_buffer))
        {
            LOG_ERROR("RDMA: Failed to recv QP Info");
            return false;
        }
        if (!Utils::SendAll(tcp_sock, send_buffer))
        {
            LOG_ERROR("RDMA: Failed to send QP Info");
            return false;
        }
    }

    // Decode remote QP Info
    size_t offset = 0;
    RdmaQpInfo remote_qp_info = DecodeQpInfo(recv_buffer.data(), offset);
    
    // Save the QP Info
    rdma_res->remote_qpn = remote_qp_info.qpn;
    rdma_res->remote_lid = remote_qp_info.lid;
    memcpy(rdma_res->remote_gid.raw, remote_qp_info.gid, 16);
    rdma_res->remote_gid_index = remote_qp_info.gid_index;

    LOG_INFO("RDMA: Exchange QP Info - Local QPN {}, GID index {}; Remote QPN {}, GID index {}, LID {}", 
        local_qp_info.qpn, rdma_res->local_gid_index, rdma_res->remote_qpn, rdma_res->remote_gid_index, rdma_res->remote_lid);

    return true;
}

// Post send WR using the provided buffer directly (no copy)
bool TransportRdma::PostSendWR(void* buffer_addr, size_t size, uint32_t lkey, bool signaled)
{
    struct ibv_sge sge;
    sge.addr = reinterpret_cast<uint64_t>(buffer_addr);
    sge.length = size;
    sge.lkey = lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    // 1 for send
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    // Notify or not
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;

    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(rdma_res->qp, &wr, &bad_wr) != 0)
    {
        LOG_ERROR("RDMA: Failed to post send WR");
        return false;
    }
    return true;
}

// Return buffer ID if available
bool TransportRdma::PollCompletion(int* out_buffer_id)
{
    struct ibv_wc wc;
    int ret = 0;

    while (true)
    {
        ret = ibv_poll_cq(rdma_res->cq, 1, &wc);
        if (ret < 0)
        {
            LOG_ERROR("RDMA: Failed to poll CQ");
            return false;
        }

        if (ret > 0)
        {
            // Check status
            if (wc.status != IBV_WC_SUCCESS)
            {
                LOG_ERROR("RDMA: Work completion is failed with status {}, {}", wc.status, ibv_wc_status_str(wc.status));
                return false;
            }

            // Get buffer id from wr_id (the lower 32b)
            if (out_buffer_id)
            {
                *out_buffer_id = static_cast<int>(wc.wr_id & 0xFFFFFFFF);
            }

            return true;
        }

        // ret == 0: No completion yet
        if (enable_busy_polling)
        {
            // Busy polling: continue immediately
            continue;
        }
        else
        {
            // Event-driven: wait for CQ event
            // Request notification on next completion
            if (ibv_req_notify_cq(rdma_res->cq, 0) != 0)
            {
                LOG_ERROR("RDMA: Failed to request CQ notification");
                return false;
            }

            // Double-check CQ before blocking (avoid race condition)
            ret = ibv_poll_cq(rdma_res->cq, 1, &wc);
            if (ret > 0)
            {
                if (wc.status != IBV_WC_SUCCESS)
                {
                    LOG_ERROR("RDMA: Work completion failed with status {}, {}", wc.status, ibv_wc_status_str(wc.status));
                    return false;
                }
                if (out_buffer_id)
                {
                    *out_buffer_id = static_cast<int>(wc.wr_id & 0xFFFFFFFF);
                }
                return true;
            }

            // Block waiting for CQ event
            struct ibv_cq* ev_cq = nullptr;
            void* ev_ctx = nullptr;
            if (ibv_get_cq_event(rdma_res->comp_channel, &ev_cq, &ev_ctx) != 0)
            {
                LOG_ERROR("RDMA: Failed to get CQ event");
                return false;
            }

            // Acknowledge the event
            ibv_ack_cq_events(ev_cq, 1);
        }
    }
}

// Manage Buffer Pool
// Allocate a free buffer, return its buffer ID
int TransportRdma::AllocateBuffer()
{
    if (!rdma_res)
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(rdma_res->buffer_mtx);
    for (auto& buf : rdma_res->buffers)
    {
        if (!buf.in_use)
        {
            buf.in_use = true;
            LOG_DEBUG("RDMA: Allocate buffer #{}", buf.id);
            return buf.id;
        }
    }

    LOG_ERROR("RDMA: No free buffer available");
    return -1;
}

void TransportRdma::ReleaseBuffer(int buffer_id)
{
    if (!rdma_res || buffer_id < 0 || buffer_id >= (int)rdma_res->buffers.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(rdma_res->buffer_mtx);
    rdma_res->buffers[buffer_id].in_use = false;
    LOG_DEBUG("RDMA: Release buffer #{}", buffer_id);
}

// Zero Copy
// Do not acquire lock, caller should protect it with send_mtx
bool TransportRdma::SendZeroCopyInternal(const void* data, size_t size)
{
    uint64_t net_size = htobe64(size);
    AutoMR size_mr(rdma_res->pd, &net_size, sizeof(uint64_t));
    if (!size_mr)
    {
        LOG_ERROR("RDMA: Failed to register size header MR to send");
        return false;
    }

    AutoMR data_mr(rdma_res->pd, const_cast<void*>(data), size);
    if (!data_mr)
    {
        LOG_ERROR("RDMA: Failed to register user buffer MR to send");
        return false;
    }

    // Send with scatter-gather list
    // sge[0] = size, sge[1] = data
    // Then send header + data together
    struct ibv_sge sge[2];
    sge[0].addr = reinterpret_cast<uint64_t>(&net_size);
    sge[0].length = sizeof(uint64_t);
    sge[0].lkey = size_mr.get()->lkey;

    sge[1].addr = reinterpret_cast<uint64_t>(data);
    sge[1].length = size;
    sge[1].lkey = data_mr.get()->lkey;

    // Prepare send WR
    struct ibv_send_wr wr = {};
    // op_type=1, upper 32b
    wr.wr_id = (1ULL << 32);
    wr.sg_list = sge;
    wr.num_sge = 2;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    // Send inline for small data
    if (size + sizeof(uint64_t) <= INLINE_THRESHOLD)
    {
        wr.send_flags |= IBV_SEND_INLINE;
    }

    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(rdma_res->qp, &wr, &bad_wr) != 0)
    {
        LOG_ERROR("RDMA: Failed to post send WR for zero copy");
        return false;
    }

    // Wait for completion
    if (!PollCompletion(nullptr))
    {
        LOG_ERROR("RDMA: Failed to poll send completion for zero copy");
        return false;
    }

    LOG_DEBUG("RDMA: Send {} bytes with zero copy", size);
    return true;
}

// Do not acquire lock, caller should protect it with recv_mtx
bool TransportRdma::RecvZeroCopyInternal(void* data, size_t size)
{
    uint64_t net_size = 0;
    AutoMR size_mr(rdma_res->pd, &net_size, sizeof(uint64_t));
    if (!size_mr)
    {
        LOG_ERROR("RDMA: Failed to register size header MR to recv");
        return false;
    }

    AutoMR data_mr(rdma_res->pd, data, size);
    if (!data_mr)
    {
        LOG_ERROR("RDMA: Failed to register user buffer MR to recv");
        return false;
    }

    // Recv with scatter-gather list
    // sge[0] = size, sge[1] = data
    struct ibv_sge sge[2];
    sge[0].addr = reinterpret_cast<uint64_t>(&net_size);
    sge[0].length = sizeof(uint64_t);
    sge[0].lkey = size_mr.get()->lkey;

    sge[1].addr = reinterpret_cast<uint64_t>(data);
    sge[1].length = size;
    sge[1].lkey = data_mr.get()->lkey;

    // Prepare recv WR
    struct ibv_recv_wr wr = {};
    // op_type=2, upper 32b
    wr.wr_id = (2ULL << 32);
    wr.sg_list = sge;
    wr.num_sge = 2;

    struct ibv_recv_wr* bad_wr = nullptr;
    if (ibv_post_recv(rdma_res->qp, &wr, &bad_wr) != 0)
    {
        LOG_ERROR("RDMA: Failed to post recv WR for zero copy");
        return false;
    }

    // Wait for completion
    if (!PollCompletion(nullptr))
    {
        LOG_ERROR("RDMA: Failed to poll recv completion for zero copy");
        return false;
    }

    // Verify the size
    size_t recv_size = be64toh(net_size);
    if (recv_size != size)
    {
        LOG_ERROR("RDMA: Size mismatch for recv zero copy. Expected {}, got {}", size, recv_size);
        return false;
    }

    LOG_DEBUG("RDMA: Recv {} bytes with zero copy", size);
    return true;
}

// Do not acquire lock, caller should protect it with send_mtx
bool TransportRdma::SendBufferPoolInternal(const void* data, size_t size)
{
    // Get one buffer to use
    int buffer_id = AllocateBuffer();
    if (buffer_id < 0)
    {
        LOG_ERROR("RDMA: Failed to allocate buffer from pool to send");
        return false;
    }

    auto& buf = rdma_res->buffers[buffer_id];
    // Check buffer size
    if (size + sizeof(uint64_t) > buf.size)
    {
        LOG_ERROR("RDMA: Data size {} + header is larger than the buffer size {}", size, buf.size);
        ReleaseBuffer(buffer_id);
        return false;
    }

    // Copy size header + data to buffer
    uint64_t net_size = htobe64(size);
    memcpy(buf.data, &net_size, sizeof(uint64_t));
    memcpy(buf.data + sizeof(uint64_t), data, size);

    // Post send WR using buffer's registered MR
    if (!PostSendWR(buf.data, size + sizeof(uint64_t), buf.mr->lkey, true))
    {
        LOG_ERROR("RDMA: Failed to post send WR");
        ReleaseBuffer(buffer_id);
        return false;
    }

    // Wait for completion
    if (!PollCompletion(nullptr))
    {
        LOG_ERROR("RDMA: Failed to poll send completion");
        ReleaseBuffer(buffer_id);
        return false;
    }

    ReleaseBuffer(buffer_id);
    
    LOG_DEBUG("RDMA: Send {} bytes with buffer pool", size);
    return true;
}

// Do not acquire lock, caller should protect it with recv_mtx
bool TransportRdma::RecvBufferPoolInternal(void* data, size_t size)
{
    // All buffers have posted recv WR
    // Using buffer_id to find which buffer
    int completed_buffer_id = -1;
    if (!PollCompletion(&completed_buffer_id))
    {
        LOG_ERROR("RDMA: Failed to poll recv completion");
        return false;
    }

    if (completed_buffer_id < 0 || completed_buffer_id >= static_cast<int>(rdma_res->buffers.size()))
    {
        LOG_ERROR("RDMA: Invalid buffer id {} from completion", completed_buffer_id);
        return false;
    }

    auto& buf = rdma_res->buffers[completed_buffer_id];
    // Read size header
    uint64_t net_size;
    memcpy(&net_size, buf.data, sizeof(uint64_t));
    size_t recv_size = be64toh(net_size);
    if (recv_size != size)
    {
        LOG_ERROR("RDMA: Recv size mismatch. Expected {}, got {}", size, recv_size);
        return false;
    }

    // Copy data to user buffer
    memcpy(data, buf.data + sizeof(uint64_t), size);

    // Re-post recv WR for this buffer again, so that it can be reused
    struct ibv_sge sge;
    sge.addr = reinterpret_cast<uint64_t>(buf.data);
    sge.length = buf.size;
    sge.lkey = buf.mr->lkey;

    // Post send WR
    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    // Upper 32b: op_type=2 for recv
    // Lower 32b: buffer_id
    wr.wr_id = (2ULL << 32) | buf.id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    struct ibv_recv_wr* bad_wr = nullptr;
    if (ibv_post_recv(rdma_res->qp, &wr, &bad_wr) != 0)
    {
        LOG_ERROR("RDMA: Failed to re-post recv WR for buffer id {}", buf.id);
        return false;
    }

    LOG_DEBUG("RDMA: Recv {} bytes with buffer id {}", size, completed_buffer_id);
    return true;
}

// Get best GID index for IB, RoCE, SoftRoCE
int TransportRdma::FindBestGidIndex(union ibv_gid* out_gid)
{
    int best_index = -1;
    memset(out_gid, 0, sizeof(union ibv_gid));

    // Check all GID table entries, select the valid GID whose index is the biggest
    // This may select RoCEv2 GID
    for (int idx = 0; idx < 16; ++idx)
    {
        union ibv_gid gid;
        if (ibv_query_gid(rdma_res->ctx, 1, idx, &gid) == 0)
        {
            // Check is valid GID (not all zero)
            bool is_valid = false;
            for (int i = 0; i < 16; ++i)
            {
                if (gid.raw[i] != 0)
                {
                    is_valid = true;
                    break;
                }
            }

            if (is_valid)
            {
                best_index = idx;
                // Copy result as output
                memcpy(out_gid, &gid, sizeof(union ibv_gid));
            }
        }
    }

    // Use index 0 as fallback if cannot find valid GID
    if (best_index < 0)
    {
        best_index = 0;
        ibv_query_gid(rdma_res->ctx, 1, best_index, out_gid);
        LOG_WARN("RDMA: No valid GID found, fallback to index 0");
    }
    else
    {
        LOG_INFO("RDMA: Select GID index {} for transport", best_index);
    }

    return best_index;
}

void TransportRdma::EncodeQpInfo(const RdmaQpInfo& qp_info, std::vector<char>& result)
{
    // Encode QPN - 4 bytes
    uint32_t qpn = qp_info.qpn;
    result.insert(result.end(), (char*)&qpn, (char*)&qpn + sizeof(qpn));

    // Encode LID - 2 bytes
    uint16_t lid = qp_info.lid;
    result.insert(result.end(), (char*)&lid, (char*)&lid + sizeof(lid));

    // Encode GID - 16 bytes
    result.insert(result.end(), (char*)qp_info.gid, (char*)qp_info.gid + 16);

    // Encode GID index - 4 bytes
    int32_t gid_index = qp_info.gid_index;
    result.insert(result.end(), (char*)&gid_index, (char*)&gid_index + sizeof(gid_index));
}

RdmaQpInfo TransportRdma::DecodeQpInfo(const char* data, size_t& offset)
{
    RdmaQpInfo qp_info;

    // Decode QPN
    memcpy(&qp_info.qpn, data + offset, sizeof(qp_info.qpn));
    offset += sizeof(qp_info.qpn);

    // Decode LID
    memcpy(&qp_info.lid, data + offset, sizeof(qp_info.lid));
    offset += sizeof(qp_info.lid);

    // Decode GID
    memcpy(qp_info.gid, data + offset, 16);
    offset += 16;

    // Decode GID Index
    memcpy(&qp_info.gid_index, data + offset, sizeof(qp_info.gid_index));
    offset += sizeof(qp_info.gid_index);

    return qp_info;
}
