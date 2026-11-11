#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# copy-config-br.py

import os
import shutil

def main():
    # 1. 脚本所在目录（workspace）
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # 2. config-brs 目录  
    config_dir = os.path.join(script_dir, "config-brs")
    if not os.path.isdir(config_dir):
        print(f"[X] 错误：未在脚本同级目录找到子目录 `config-brs`")
        return

    # 3. 输入编号
    idx = input("请输入配置编号（例如 01、51、122）: ").strip()
    src_name = f"config-br-{idx}"
    src_path = os.path.join(config_dir, src_name)

    if not os.path.isfile(src_path):
        print(f"[X] 错误：在 `config-brs` 中未找到文件 `{src_name}`")
        return

    # 4. 目标目录就是 script_dir 自身
    dst_dir = script_dir

    # 5. 复制并重命名为 config-br
    try:
        shutil.copy(src_path, os.path.join(dst_dir, "config-br"))
        print(f"[Y] 成功：已将 `{src_name}` 复制到 `{dst_dir}` 并命名为 `config-br`")
    except Exception as e:
        print(f"[X] 复制 config-br 失败：{e}")
        return

    # 6. 在 dst_dir 创建空文件 br.sqlite
    try:
        open(os.path.join(dst_dir, "br.sqlite"), 'w').close()
        print(f"[Y] 成功：已在 `{dst_dir}` 下创建空文件 `br.sqlite`")
    except Exception as e:
        print(f"[X] 创建 br.sqlite 失败：{e}")

    # 7. 生成 arp-address，内容 BR<x>-<y>
    try:
        x = idx[:-1] if len(idx) > 1 else ""
        y = idx[-1]
        content = f"BR{x}-{y}"
        with open(os.path.join(dst_dir, "arp-address"), 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"[Y] 成功：已在 `{dst_dir}` 下创建 `arp-address`，内容：{content}")
    except Exception as e:
        print(f"[X] 创建 arp-address 失败：{e}")

if __name__ == "__main__":
    main()
