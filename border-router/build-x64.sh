#!/bin/sh

case "$1" in
    "reconf") 
        rm -r build
    ;;
    "clean") 
        cd build
        make clean
        exit 0
    ;;
esac

if [ ! -d "build" ]; then
    mkdir build
    cd build
    cmake -DPERF_TEST=ON ..
    cd ..
fi

cmake --build build/ --target br -- -j8
cmake --build build/ --target br-pipeline0 -- -j8
