#!/bin/bash

if [ $EUID -ne 0 ]; then
    echo "Please run as root"
    exit
fi

PS3="Please select your choice: "

options=("Download configuration files" "Clean up and quit" "10^1" "10^2" "10^3" "10^4" "10^5" "10^6" "10^7" "Quit")

select option in "${options[@]}"
do
    case $REPLY in
        1) sudo -H -u zeusnet sh -c "scp -r zeusnet@10.134.149.206:~/Desktop/color-border-router/test/test3-nidt/generated-configs/br* /home/zeusnet/Desktop/color-border-router/tmp/"; exit 0;;
        2)
            if [ -d "tmp" ]; then 
                rm -r tmp
            fi
            exit 0
        ;;
        3) config_path="./tmp/br-config-10/"; break;;
        4) config_path="./tmp/br-config-100/"; break;;
        5) config_path="./tmp/br-config-1000/"; break;;
        6) config_path="./tmp/br-config-10000/"; break;;
        7) config_path="./tmp/br-config-100000/"; break;;
        8) config_path="./tmp/br-config-1000000/"; break;;
        9) config_path="./tmp/br-config-10000000/"; break;;
        10) exit 0;;
    esac
done

sudo -H -u zeusnet sh -c "./build-x64.sh"

set -x # echo on

./build/br-pipeline0 -c 0x0f --log-level 6 -- -p 0x3F --config-file "$config_path"config-br --db-file "$config_path"br.sqlite
