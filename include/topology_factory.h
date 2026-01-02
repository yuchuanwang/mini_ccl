// topology_factory.h
// Factory to different topology instance
// 

#pragma once

#include <memory>
#include "types.h"
#include "logger.h"
#include "topology.h"
#include "topology_fullmesh.h"
#include "topology_ring.h"

class TopologyFactory
{
public:
    static std::shared_ptr<Topology> Create(TopologyType type)
    {
        switch (type)
        {
        case TopologyType::FULL_MESH:
            return std::make_shared<TopologyFullMesh>();

        case TopologyType::RING:
            return std::make_shared<TopologyRing>();
        
        default:
            LOG_ERROR("Unsupported topology type {}", static_cast<int>(type));
            return nullptr;
        }
    };

    static const char* GetTypeName(TopologyType type)
    {
        switch (type)
        {
        case TopologyType::FULL_MESH:
            return "FullMesh";

        case TopologyType::RING:
            return "Ring";
        
        default:
            return "Unknown";
        }
    }
};
