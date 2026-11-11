#!/bin/bash
# 原部署版为 cd /home/color/border-router；改为相对脚本所在目录，便于在任意路径/容器中使用
cd "$(dirname "$(readlink -f "$0")")/border-router" || exit 1
rm -rf build
meson setup build
meson compile -C build

