#!/bin/bash
# 文件名：kill-br.sh

# 获取当前目录下br的绝对路径
EXE_PATH="$(realpath ./br)"

# 找到所有正在运行且可执行文件路径等于EXE_PATH的进程
for pid in $(pgrep -x br); do
    # 检查进程对应的可执行文件路径
    if [ "$(readlink -f /proc/$pid/exe 2>/dev/null)" = "$EXE_PATH" ]; then
        kill "$pid" && echo "已强制停止进程 PID $pid"
    fi
done
