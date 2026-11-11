# 欢迎使用新版边界路由器
## 一、环境准备
类似于新版资源管理器the-new-rm，dpdk安装是完全一样的，这里不再赘述，您可以直接查看the-new-rm的README。

除了dpdk，BR的运行还依赖两个额外的库，分别是sqlite3和openssl，这两个库您可以直接apt install libsqlite3-dev libssl(好像是这个名字，查一下就行)，也可以从源码编译安装，在咱们实验室gitlab color栏目下我提供了源码和安装教程。如果您安装后编译br发现版本报错，那么直接改一下meson.build文件里的版本，sqlite3和openssl的版本都不碍事，dpdk必须是23LTS。

**目前不需要把东西拷贝到~/目录里了，一切都只在项目根目录下，包括各种配置文件，一律使用相对路径**

~~在BR源码的根目录下有一个子目录```prepare/```，里面的文件全部拷贝到~/下。~~

大页内存网卡配置什么的在新版RM的文档和脚本注释里面说的很清楚，您可以直接看一下，都是类似的，很好理解，这里不再赘述。

~~记得把BR源码文件夹也拷贝到```~/```里，最后您的```~/```下应该有这些文件~~
```
the-new-br/
arp-address
br.sqlite
config-br
dpdk-devbind.py
dpdk-hugepages.py
re-build-br.sh
run-br.sh
set-br.sh
```

## 二、使用方法
和RM类似。

当您开机/重启时，执行一次```./set-br.sh```

当您需要重新编译BR源码时，执行一次```./re-build-br.sh```

当您需要运行BR时，执行一次```./run-br.sh```

注意我们运行所有命令时，终端都应该位于```~/```目录下(RM也是一样)