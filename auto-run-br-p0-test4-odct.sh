#!/bin/bash

if [ $EUID -ne 0 ]; then
    echo "Please run as root"
    exit
fi

PS3="Please select your choice: "

options=(
    "Download configuration files" 
    "Clean up and quit" 
    "ODC enabled, IDHV enabled"
    "ODC enabled, IDHV disabled"
    "Quit"
)

br_config_dir_root="tmp"

select option in "${options[@]}"
do
    case $REPLY in
        1) 
            if [ -d $br_config_dir_root ]; then 
                rm -r $br_config_dir_root
            fi
            sudo -H -u zeusnet sh -c "mkdir $br_config_dir_root"
            sudo -H -u zeusnet sh -c "scp -r zeusnet@10.134.149.206:~/Desktop/color-border-router/test/test4-odct/generated-configs/br-config* /home/zeusnet/Desktop/color-border-router/$br_config_dir_root"
            exit 0
        ;;
        2)
            if [ -d $br_config_dir_root ]; then 
                rm -r $br_config_dir_root
            fi
            exit 0
        ;;
        3) additional_config="\nodc enable\nidhv enable\n"; break;;
        4) additional_config="\nodc enable\nidhv disable\n"; break;;
        5) 
            ps -ax | grep "root@10.134.149.206" | awk '{print $1}' |xargs kill -9 $1
            exit 0;;
    esac
done

sudo -H -u zeusnet sh -c "./build-x64.sh"

remote_test_dir="/home/zeusnet/Desktop/color-border-router/test/test4-odct/"
remote_host="root@10.134.149.206"
config_path_base="$br_config_dir_root/br-config-"
odct_size=(10 100 1000 10000 100000 1000000 10000000)
# odct_size=(10000000)

for i in {1..10}
do
    for size in "${odct_size[@]}"
    do
        db_file="${config_path_base}${size}/br.sqlite"
        config_br="${config_path_base}${size}/config-br"
        config_br_tmp="${config_path_base}${size}/config-br-tmp"
        printf "${additional_config}"
        cp $config_br $config_br_tmp
        printf "${additional_config}" >> $config_br_tmp
        cmd="./build/br-pipeline0 -c 0x0f --log-level 6 -- -p 0x3F --config-file $config_br_tmp --db-file $db_file"
        br_init_seconds=$([ $size -le 1000000 ] && echo 15 || echo 90)
        br_running_seconds=$(echo "$br_init_seconds+180" | bc)
        remote_cmd="ssh -t $remote_host \"cd $remote_test_dir; sleep $br_init_seconds; ./run.py $size 2>/dev/null > /dev/null\""
        echo "repeat $i"
        echo "${cmd}"
        echo "${remote_cmd}"
        sudo -H -u zeusnet sh -c "$remote_cmd" &
        timeout --foreground ${br_running_seconds}s $cmd
        rm $config_br_tmp
        reset
    done
done