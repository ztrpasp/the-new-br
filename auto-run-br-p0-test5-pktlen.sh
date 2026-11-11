#!/bin/bash

if [ $EUID -ne 0 ]; then
    echo "Please run as root"
    exit
fi

PS3="Please select your choice: "

options=(
    "Download configuration files" 
    "Clean up and quit" 
    "Start" 
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
            sudo -H -u zeusnet sh -c "scp -r zeusnet@10.134.149.206:~/Desktop/color-border-router/test/test5-pktlen/generated-configs/br* /home/zeusnet/Desktop/color-border-router/$br_config_dir_root"
            exit 0
        ;;
        2)
            if [ -d $br_config_dir_root ]; then 
                rm -r $br_config_dir_root
            fi
            exit 0
        ;;
        3) break;;
        4) exit 0;;
    esac
done

sudo -H -u zeusnet sh -c "./build-x64-pktlen-test.sh"

remote_test_dir="/home/zeusnet/Desktop/color-border-router/test/test5-pktlen/"
remote_host="root@10.134.149.206"
config_path_base="$br_config_dir_root/br-config"
additional_config="\nodc disable\nidhv disable\n\n"
num_pids_list=($(seq 1 1 16))
num_pcl_list=(0 1 2 3) # prefeteched_cache_lines

num_repeat=1
for i in $(seq 1 1 ${num_repeat})
do
    for num_pids in "${num_pids_list[@]}"
    do
        db_file="${config_path_base}/br.sqlite"
        config_br="${config_path_base}/config-br"
        config_br_tmp="${config_path_base}/config-br-tmp"
        for num_pcl in "${num_pcl_list[@]}"
        do
            cp $config_br $config_br_tmp
            printf "${additional_config}" >> $config_br_tmp
            br_init_seconds=15
            br_running_seconds=$(echo "$br_init_seconds+150" | bc)
            cmd="./build-pktlen-test-pcl-${num_pcl}/br-pipeline0 -c 0x1f --log-level 6 -- -p 0x3F --config-file $config_br_tmp --db-file $db_file"
            remote_cmd="ssh -t $remote_host \"cd $remote_test_dir; sleep $br_init_seconds; ./run-test.py $num_pids $num_pcl 2>/dev/null 1>/dev/null\""
            echo "repeat $i"
            echo "${cmd}"
            echo "${remote_cmd}"
            sudo -H -u zeusnet sh -c "$remote_cmd" &
            timeout --foreground ${br_running_seconds}s $cmd
            rm $config_br_tmp
            reset
        done
    done
done
