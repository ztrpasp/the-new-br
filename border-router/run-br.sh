#!/bin/sh

sudo -H -u zeusnet sh -c "./build-x64.sh"
# --config (port,queue,lcore)[,(port,queue,lcore)]
./build/br -c 0x7f --log-level 6 -- -p 0x3F -P --config="(0,0,1),(1,0,2),(2,0,3),(3,0,4),(4,0,5),(5,0,6),(5,1,1)" --config-file ./config-br
