// transport factory.h
// Factory to different transport instance
// 

#pragma once

#include <memory>
#include "types.h"
#include "logger.h"
#include "transport.h"
#include "transport_tcp.h"
#include "transport_rdma.h"

class TransportFactory
{
public:
    static std::shared_ptr<Transport> Create(TransportType type)
    {
        switch (type)
        {
        case TransportType::TCP:
            return std::make_shared<TransportTCP>();

        case TransportType::RDMA:
            return std::make_shared<TransportRdma>();
        
        default:
            LOG_ERROR("Unsupported transport type {}", static_cast<int>(type));
            return nullptr;
        }
    };

    static const char* GetTypeName(TransportType type)
    {
        switch (type)
        {
        case TransportType::TCP:
            return "TCP";

        case TransportType::RDMA:
            return "RDMA";
        
        default:
            return "Unknown";
        }
    }
};
