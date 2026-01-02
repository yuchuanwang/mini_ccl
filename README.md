# Mini CCL

A lightweight, educational collective communication library supporting both TCP and RDMA transports.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)

## Overview

**Mini CCL (Collective Communication Library)** is designed for learning distributed systems, RDMA programming, and collective communication algorithms. With ~3000 lines of C++, it implements complete collective communication primitives used in distributed machine learning frameworks like NCCL.

### Key Features

- 🔗 **Dual Transport**: TCP (standard sockets) + RDMA (`libibverbs`)
- 🔄 **Ring Topology**: Efficient O(N) Ring AllReduce algorithm
- 📦 **Buffer Pool**: Pre-registered memory for low-latency RDMA
- 🚀 **Zero Copy**: Direct user buffer registration for large data
- ⚡ **Busy/Event Polling**: Configurable CQ polling modes
- 🛡️ **Modern C++17**: RAII, smart pointers, inline initialization

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Layer                          │
│                  (User programs, test_ccl.cpp)                  │
├─────────────────────────────────────────────────────────────────┤
│                     Communicator Layer                          │
│         Unified API (AllReduce, Broadcast, Gather, ...)         │
├─────────────────────────────────────────────────────────────────┤
│                      Topology Layer                             │
│    ┌─────────────────────┬─────────────────────┐                │
│    │    TopologyRing     │  TopologyFullMesh   │                │
│    │  O(N) complexity    │   O(N²) complexity  │                │
│    └─────────────────────┴─────────────────────┘                │
├─────────────────────────────────────────────────────────────────┤
│                      Transport Layer                            │
│    ┌─────────────────────┬─────────────────────┐                │
│    │    TransportTCP     │   TransportRdma     │                │
│    │  Standard Socket    │   libibverbs API    │                │
│    └─────────────────────┴─────────────────────┘                │
└─────────────────────────────────────────────────────────────────┘
```

## Supported Collective Operations

| Operation | Description | Communication Pattern |
|-----------|-------------|----------------------|
| **Broadcast** | Root sends data to all ranks | One-to-Many |
| **Scatter** | Root distributes different chunks to each rank | One-to-Many |
| **Gather** | All ranks send data to root | Many-to-One |
| **Reduce** | Reduce data to root (SUM/MAX/MIN/AVG) | Many-to-One |
| **AllGather** | All ranks collect data from all ranks | Many-to-Many |
| **ReduceScatter** | Reduce and scatter to all ranks | Many-to-Many |
| **AllReduce** | All ranks hold the reduced result | Many-to-Many |
| **AllToAll** | Full exchange | Many-to-Many |
| **Barrier** | Synchronization barrier | Synchronization |

## Requirements

### Build Dependencies

- CMake >= 3.10
- C++17 compatible compiler (GCC 7+, Clang 5+)
- fmt library
- pthreads
- libibverbs (for RDMA support)

### RDMA Requirements

For RDMA functionality, you need either:
- **Hardware**: Mellanox/NVIDIA ConnectX RDMA NIC
- **Software**: Soft-RoCE (rxe) for development/testing

## Quick Start

### Installation

```bash
# Ubuntu/Debian
sudo apt install -y libibverbs-dev ibverbs-utils rdma-core libfmt-dev mpich

# Clone and build
git clone https://github.com/yuchuanwang/mini_ccl.git
cd mini_ccl
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Setup Soft-RoCE (Optional, for testing without RDMA hardware)

```bash
# Load kernel module
sudo modprobe rdma_rxe

# Create RXE device on your network interface
sudo rdma link add rxe0 type rxe netdev eth0

# Verify
rdma link
```

### Running Tests

**Using MPI launcher:**

```bash
# TCP + Ring (default)
mpirun -np 4 ./test_ccl
# RDMA + Ring
mpirun -np 4 ./test_ccl rdma
# TCP + FullMesh
mpirun -np 4 ./test_ccl tcp fullmesh
```

**Manual multi-terminal mode:**

```bash
# Terminal 1 (Rank 0)
./tests/test_ccl 0 4 rdma ring

# Terminal 2-4 (Rank 1-3)
./tests/test_ccl 1 4 rdma ring
./tests/test_ccl 2 4 rdma ring
./tests/test_ccl 3 4 rdma ring
```


## Project Structure

```
mini_ccl/
├── include/              # Header files
│   ├── bootstrap.h       # Node discovery and cluster setup
│   ├── communicator.h    # Main API entry point
│   ├── topology.h        # Topology interface
│   ├── transport.h       # Transport interface
│   ├── transport_tcp.h   # TCP transport
│   ├── transport_rdma.h  # RDMA transport
│   ├── types.h           # Common types and configs
│   ├── utils.h           # Utility functions
│   └── logger.h          # Logging system
├── src/                  # Implementation files
│   ├── bootstrap.cpp
│   ├── communicator.cpp
│   ├── topology_ring.cpp      # Ring topology algorithms
│   ├── topology_fullmesh.cpp  # Full mesh topology
│   ├── transport_tcp.cpp
│   ├── transport_rdma.cpp
│   └── utils.cpp
├── tests/                # Test programs
│   ├── test_ccl.cpp      # Main collective communication test
│   ├── test_polling.cpp  # Busy vs Event-driven polling comparison
│   └── test_zero_copy.cpp # Zero Copy vs Buffer Pool comparison
├── docs/                 # Documentation
├── CMakeLists.txt
└── README.md
```

## Usage Example

```cpp
#include "communicator.h"
#include "types.h"

int main() {
    mini_ccl::CommConfig config;
    config.rank = 0;
    config.world_size = 4;
    config.master_addr = "127.0.0.1";
    config.master_port = 12321;
    config.transport = "rdma";  // or "tcp"
    config.topology = "ring";   // or "fullmesh"

    mini_ccl::Communicator comm;
    if (!comm.Init(config)) {
        return 1;
    }

    // AllReduce example
    std::vector<float> data(1024, config.rank + 1.0f);
    std::vector<float> result(1024);

    comm.AllReduce(data.data(), result.data(), 1024,
                   mini_ccl::DataType::FLOAT,
                   mini_ccl::ReduceOp::SUM);

    // Each rank now has sum: 1+2+3+4 = 10
    
    comm.Finalize();
    return 0;
}
```

## RDMA Features

### Buffer Pool Mode

Pre-registered memory buffers for low-latency small message transfers:
- 16 buffers × 16 MB each (configurable)
- Eliminates per-transfer MR registration overhead
- Best for messages < 1 MB

### Zero Copy Mode

Direct user buffer registration for large data transfers:
- No intermediate copy
- Higher throughput for large messages
- Best for messages >= 1 MB

### Polling Modes

| Mode | Latency | CPU Usage | Use Case |
|------|---------|-----------|----------|
| Busy Polling | ~50 µs | ~100% | Latency-sensitive |
| Event-Driven | ~85 µs | ~12% | Resource-constrained |

## Performance Benchmarks

Run the benchmark tools:

```bash
# Compare polling modes
./tests/test_polling

# Compare Zero Copy vs Buffer Pool
./tests/test_zero_copy server  # Terminal 1
./tests/test_zero_copy client  # Terminal 2
```

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Inspired by NVIDIA NCCL
- Thanks to the RDMA community for libibverbs documentation

---

**If you find this project helpful, please give it a ⭐!**

