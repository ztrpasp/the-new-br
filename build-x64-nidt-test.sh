#!/bin/sh

case "$1" in
    "reconf") 
        rm -r build-nidt-test
    ;;
    "clean") 
        cd build-nidt-test
        make clean
        exit 0
    ;;
esac

if [ ! -d "build-nidt-test" ]; then
    mkdir build-nidt-test
    cd build-nidt-test
    cmake -DPERF_TEST=ON -DNIDT_TEST=ON ..
    cd ..
fi

cmake --build build-nidt-test/ --target br-pipeline0 -- -j8
