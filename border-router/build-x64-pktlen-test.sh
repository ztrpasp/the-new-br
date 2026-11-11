#!/bin/bash

n=3
options=("PREFETCH_NONE" "PREFETCH_ONE" "PREFETCH_TWO" "PREFETCH_THREE")

case "$1" in
    "reconf") 
        for i in $(seq 0 1 ${n})
        do
            rm -r build-pktlen-test-pcl-${i}
        done
    ;;
    "clean")
        for i in $(seq 0 1 ${n})
        do
            cd build-pktlen-test-pcl-${i}
            make clean
            cd ..
        done
        exit 0
    ;;
esac

for i in $(seq 0 1 ${n})
do
    if [ ! -d "build-pktlen-test-pcl-${i}" ]; then
        mkdir build-pktlen-test-pcl-${i}
        cd build-pktlen-test-pcl-${i}
        cmake -DPERF_TEST=ON -D${options[${i}]}=ON ..
        cd ..
    fi
    cmake --build build-pktlen-test-pcl-${i}/ --target br-pipeline0 -- -j8
done
