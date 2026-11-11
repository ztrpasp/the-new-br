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
            sudo -H -u zeusnet sh -c "scp -r zeusnet@10.134.149.206:~/Desktop/color-border-router/test/test3-nidt/generated-configs/br* /home/zeusnet/Desktop/color-border-router/$br_config_dir_root"
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

sudo -H -u zeusnet sh -c "./build-x64-nidt-test.sh"

remote_test_dir="/home/zeusnet/Desktop/color-border-router/test/test3-nidt/"
remote_host="root@10.134.149.206"
config_path_base="$br_config_dir_root/br-config-"
sizes=(1000 10000 100000)
nidt_params_add_rate=(0.01 1 2 3 4 5 6 7 8 9)

for i in {1..10}
do
    for size in "${sizes[@]}"
    do
        db_file="${config_path_base}${size}/br.sqlite"
        config_br="${config_path_base}${size}/config-br"
        config_br_tmp="${config_path_base}${size}/config-br-tmp"
        nidt_additional_entries=$(echo "$size/10" | bc) # 10%
        for nidt_add_rate in "${nidt_params_add_rate[@]}"
        do
            nidt_add_rate=$(echo "$nidt_add_rate*$size/10" | bc)
            nidt_expire_seconds=$(echo "scale=3;$nidt_additional_entries/$nidt_add_rate" | bc)
            nidt_param="$nidt_add_rate $nidt_expire_seconds"
            additional_config="\nodc disable\nidhv disable\n\n@nidt-test ${nidt_param}\n"
            printf "${additional_config}"
            cp $config_br $config_br_tmp
            printf "${additional_config}" >> $config_br_tmp
            br_init_seconds=$([ $size -le 1000000 ] && echo 15 || echo 90)
            br_running_seconds=$(echo "$br_init_seconds+90" | bc)
            cmd="./build-nidt-test/br-pipeline0 -c 0x1f --log-level 6 -- -p 0x3F --config-file $config_br_tmp --db-file $db_file"
            remote_cmd="ssh -t $remote_host \"cd $remote_test_dir; sleep $br_init_seconds; ./run-nidt-test.py $size $nidt_add_rate 2>/dev/null > /dev/null\""
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
