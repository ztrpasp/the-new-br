#!/bin/bash
./build/br -c 0xf --log-level 7 -- -p 0x7 -P --config="(0,0,0),(1,0,1),(2,0,2)" --config-file ./config-br
# 含义解析：
# -c 0xf : 您想启用的内核掩码，这里0xf就是二进制1111，表示启用四个内核（lcore0-3）
# --log-level 7 : dpdk运行日志等级
# -- 分割线，左边是dpdk eal参数，右边是您自己的br程序设定的参数
# --config="(<port_id>,<queue_id=0>,<lcore_id>)" : 这个表示您准备让哪个核管哪个端口的0队列（一般来说我们就用队列0就OK）
# --config-file ./config-br : config-br位置