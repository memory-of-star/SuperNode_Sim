# ns-3.47 使用说明

源码目录：`/home/dell/scale_up_evaluation/ns-3.47`

官方下载页：https://www.nsnam.org/releases/ns-3-47/download/

下载地址：https://www.nsnam.org/release/ns-3.47.tar.bz2

压缩包保留在当前目录，SHA-1 已与官网核对：`897c3f2db2c6de6a8291e46b3020dbeb1af75269`。

## 运行示例

```bash
cd /home/dell/scale_up_evaluation/ns-3.47
./ns3 run hello-simulator
./ns3 run first
```

`hello-simulator` 输出 `Hello Simulator`。

`first` 模拟两个节点通过 5 Mbps、单向传播时延 2 ms 的点对点链路通信。客户端在仿真时间 2 秒发送 1024 字节 UDP 数据，服务端回显，客户端在约 2.00737 秒收到回复。

示例源码位于 `ns-3.47/examples/tutorial/first.cc`。自定义 C++ 仿真可以放在 `ns-3.47/scratch/`，再用 `./ns3 run 程序名` 编译运行。

## 本次构建与验证

使用 GCC 12.3.0、CMake 3.22.1、Ninja，完成以下构建：

```bash
./ns3 configure --enable-examples --enable-tests
./ns3 build -j 24
```

配置启用的默认模块、示例及测试程序均已编译成功。没有进行系统级安装；可直接在源码目录运行。源码和构建产物共约 3.4 GB。

两个示例均运行成功，以下三个内置测试套件均通过（未运行完整测试集）：

```bash
python3 test.py --no-build --suite=simulator
python3 test.py --no-build --suite=packet
python3 test.py --no-build --suite=udp
```

Python 绑定、PyViz、MPI，以及依赖未安装的 BRITE、Click、OpenFlow 等可选扩展未启用；当前配置支持 C++ 仿真。

配置、编译、示例输出及测试日志保存在 `ns-3.47/setup-*.log`。
