# 按 cgroup 限制网络发送速率

## 用法

安装 `traffic-limitd` 软件包。

``` bash
apt-get install ./traffic-limitd_0.0.1-1xxxxxxx.deb
```

然后编辑配置文件 `/etc/defaults/traffic-limitd`，设置 `IFACE` 变量为需要进行流控的网卡。

最后用命令 `systemctl status traffic-limitd.socket` 确认控制端口是开放的。

在安装完毕后，用户可以使用下列命令来开启流量受限制的任务。

``` bash
traffic-limit --packet-rate xxm --bit-rate xxg python some_script.py
```

其中，`--packet-rate` 和 `--bit-rate` 为可选参数，分别用于限制网络发送速率和流量。当不指定时，表示不限制。其中所指定的限速参数，可以后跟 SI 词缀。例如：`--bit-rate 1g` 表示限制 1 Gbps 发送流量；`--packet-rate 1k` 表示限制 1 kpps 发包速率。被启动的程序可以是任意命令，例如，可以是具体的发送网络数据的程序，也可以是启动一个 shell，然后再在 shell 中进行进一步操作。

## 已知问题

- 本程序不能限制接收网络数据的速率；
- 本程序禁止递归调用；
- 本程序暂时不能排队；
- 本程序同时只能服务 1000 个任务，超过时会被直接拒绝；
- 本程序后台进程崩溃时，所有的任务将被强行终止；
- 当被启动的任务的主进程终止时，由主进程所启动所有进程，无论其父进程是否时该进程，如果尚未退出，将被强行终止。
