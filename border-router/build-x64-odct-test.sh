#!/bin/sh

case "$1" in
    "reconf") 
        rm -r build-odct-test
    ;;
    "clean") 
        cd build-odct-test
        make clean
        exit 0
    ;;
esac

if [ ! -d "build-odct-test" ]; then
    mkdir build-odct-test
    cd build-odct-test
    cmake -DPERF_TEST=ON -DODCT_TEST=ON ..
    cd ..
fi

cmake --build build-odct-test/ --target br-pipeline0 -- -j8
