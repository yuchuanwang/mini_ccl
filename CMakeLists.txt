cmake_minimum_required(VERSION 3.10)
project(mini_ccl VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED on)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g")

find_package(Threads REQUIRED)
find_package(fmt REQUIRED)

find_path(IBV_INCLUDE_DIR infiniband/verbs.h)
find_library(IBV_LIBRARY ibverbs)

# Source files
set(MINI_CCL_SOURCES
    src/utils.cpp
    src/transport_tcp.cpp
    src/transport_rdma.cpp
    src/topology_fullmesh.cpp
    src/topology_ring.cpp
    src/bootstrap.cpp
    src/communicator.cpp
)

add_library(mini_ccl SHARED ${MINI_CCL_SOURCES})

target_include_directories(mini_ccl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${IBV_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mini_ccl
    PRIVATE
        Threads::Threads
        fmt::fmt
        ${IBV_LIBRARY}
)

# TODO: Binding

add_subdirectory(tests)
#add_subdirectory(samples)
