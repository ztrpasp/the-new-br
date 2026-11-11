#!/bin/bash
python3 ./dpdk-hugepages.py --pagesize 1G --setup 2G
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1
ifconfig enp3s0 down
ifconfig enp4s0 down
ifconfig enp5s0 down
python3 ./dpdk-devbind.py -b vfio-pci enp3s0 enp4s0 enp5s0

