# 新版边界路由器（BR）

本仓库的目录结构、脚本和配置文件与实际部署的 BR 一致，源码与部署机上运行的版本相同。

## 来源

- `border-router/`：实验室 GitLab `color/border-router` 仓库，`IPv6Extension` 分支 `afbe26b`（upgrade to dpdk23LTS），外加部署机上未提交的修改：
  - `br.h`：增加收包调试打印
  - `init.c`：开启 RX/TX IPv4 校验和 offload
  - `br-rtc.c`：仅注释
- 顶层文件和 `br/`：从部署机原样拷贝。只有 `re_build_br.sh` 做了改动：原来写死的 `/home/color` 路径改成了相对路径。

## 目录结构

```
.
├── border-router/      BR 源码（meson 构建，产物为 border-router/build/br）
├── br/                 静态链接版的部署包（目标机无需安装 DPDK/OpenSSL/sqlite）
│   ├── br              静态可执行文件，不入库，需另行放入
│   ├── config-br       当前节点配置
│   ├── config-br-121   节点 121 的配置（与 config-brs/ 中同名文件 IP 不同，为现场修改版）
│   ├── config-brs/     各节点配置模板
│   ├── copy-config-br.py  选择节点配置，生成 config-br / br.sqlite / arp-address
│   ├── set-br.sh       开机后执行一次：配置大页内存，并把网卡绑定到 vfio-pci
│   ├── run-br.sh       启动 BR
│   └── kill-br.sh      停止 BR
├── br-guide.txt        原部署使用说明
├── config-br           源码编译版使用的配置
├── arp-address / br.sqlite
├── dpdk-devbind.py / dpdk-hugepages.py
├── re_build_br.sh      编译 border-router/
└── run-br.sh           运行 border-router/build/br
```

## 编译

依赖（Ubuntu 22.04）：

```bash
apt install build-essential pkg-config python3-pyelftools meson ninja-build net-tools libnuma-dev
```

- **DPDK 23.11 LTS**：从源码执行 `meson setup build && meson compile -C build && meson install -C build && ldconfig`。部署版用的是 23.11.4。
- **sqlite3 >= 3.38.0**、**OpenSSL 1.1.1m**：版本在 `border-router/meson.build` 中写死。Ubuntu 22.04 用 apt 装到的是 sqlite 3.37 和 OpenSSL 3.0，版本检查不通过，部署版是从源码安装的这两个库（sqlite 3.38.2，OpenSSL 装在 `/usr/local`）。

```bash
./re_build_br.sh          # 产物：border-router/build/br
```

编译选项见 `border-router/meson_options.txt`。默认 `perf_test=true`，此时直接使用网卡真实 MAC，不读取 `arp-address`。

`idle_sleep_us`（默认 0）：收包循环连续空转若干轮后睡这么多微秒，命令行线程空闲时也睡 1 ms。真机保持 0（忙轮询，与原来一样）；容器实验环境（color-lab）设成 50，让空闲的 BR 不占满 CPU，代价是空闲后第一个包多几十微秒时延。

## 运行

所有操作都需要 root 权限，并且必须**在配置文件所在目录下执行**：程序按相对路径读取 `./config-br`、`./br.sqlite`。

```bash
# 源码编译版（顶层目录）
./run-br.sh               # 网卡绑定可参考 br/set-br.sh

# 静态版（br/ 目录）
cd br
python3 copy-config-br.py # 可选：按节点编号选取配置
./set-br.sh               # 每次开机执行一次；网卡名 enp2s0/enp3s0/enp4s0 需按本机修改
./run-br.sh               # Ctrl+C 或 ./kill-br.sh 停止
```

`run-br.sh` 参数说明：

- `-c`：EAL 核掩码
- `-p`：DPDK 端口掩码
- `-P`：开启混杂模式
- `--config="(端口,队列,核)"`：端口队列与核的对应关系
- `--config-file`：配置文件路径

`-p` 和 `--config` 要与实际绑定的网卡数一致。

## 配置文件 config-br

| 配置项 | 含义 |
|---|---|
| `sysname` | 设备名 |
| `br-nid` | 本 BR 的 NID（16 字节） |
| `debugging on` / `l2 on` / `l3 on` | 调试输出 |
| `odc` / `idhv` `enable\|disable` | ODC / IDHV 功能开关 |
| `arp enable` | 启用 ARP |
| `rm ip address <IP>` | 资源管理器 RM 的地址 |
| `sk <key>` | 本域密钥（用于 PID 计算） |
| `port <n> ip address <IP/len>` | DPDK 端口地址 |
| `peer add external <IP> sk <K>` | 邻居 BR |
| `peer add-path <IP> as <N> px <hex>/<len>` | 邻居的 AS / PX |
| `nid <hex> <IP>` | NID-IP 映射 |
| `ip-route <前缀> <下一跳>` | 静态路由 |

部署中的配置文件都是 CRLF 换行，BR 可以正常解析。`copy-config-br.py` 也是 CRLF 换行，请用 `python3 copy-config-br.py` 运行，不要直接 `./` 执行。
