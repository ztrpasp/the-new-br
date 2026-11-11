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
    cmake -DCROSS_COMPILE=ON -DPERF_TEST=OFF ..
    cd ..
fi

cd ./build
make br-static -j 8
chmod +x ./br-static
cp ./br-static $HOME/tftp/br


#cmake --build build/ --target br-static -- -j8

#cp ./build/br-static $HOME/tftp/br

#echo "./br -c 0x1f --log-level 8 -- -p 0xF0 -P --config=\"(4,0,0),(5,0,1),(6,0,2),(7,0,3)\"" > $HOME/tftp/run-br.sh
