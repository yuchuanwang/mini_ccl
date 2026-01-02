#!/bin/bash

# Create Soft RoCE
sudo modprobe rdma_rxe

sudo rdma link add rxe0 type rxe netdev lo
ibv_devices

# Delete device
#sudo rdma link delete rxe0
