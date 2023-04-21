# 按 cgroup 限制网络发送速率

## 依赖

- `bpftool` >= 5.5
- `tc`
- `systemd-run`
- `sudo`
- `findmnt`
- `numfmt`
- `nsenter`

## 用法

首先先在出接口上载入流控框架，假定出接口名称是 `eth0`，需要使用 root 权限执行：

``` bash
cd /path/to/cgroup-rate-limit
./scheduler.sh load eth0
```
流控框架载入后，即可使用 `start_limited.sh` 启动相应程序，限制网络发送速率/流量：

``` bash
/path/to/cgroup-rate-limit/start_limited.sh --pps xxm --bps xxg python some_script.py
```

其中，`--pps` 和 `--bps` 为可选参数，分别用于限制网络发送速率和流量。当不指定时，表示不限制。其中所指定的限速参数，可以后跟 SI 词缀。例如：`--bps 1g` 表示限制 1 Gbps 发送流量；`--pps 1k` 表示限制 1 kpps 发包速率。被启动的程序可以是任意命令，例如，可以是具体的发送网络数据的程序，也可以是启动一个 shell，然后再在 shell 中进行进一步操作。

## 已知问题

- `start_limited.sh` 脚本中，设定限速速率需要 root 权限。

   在脚本执行过程中，如果检测到该脚本并非以 root 权限执行，则会自动调用 `sudo` 提升权限。设定限速速率后，自动降低回原权限继续执行所指定的命令。

- 本脚本不能限制接收网络数据的速率。
