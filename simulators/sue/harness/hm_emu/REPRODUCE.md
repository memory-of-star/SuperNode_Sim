# Kimi K3 Scale-Up报告复现

本文对应报告的当前版本：**SUE逐包网络、卡间树形AllReduce、MoE共享/路由双分支并行**。第二章仅含2/4/8卡；第三章仅分析B=1、TP8；第四章为同一实现下的MTP评估。报告不展示其他集合通信调度方式的性能对照。

所有命令在仓库根目录执行。Git包含输入、代码、HTML及汇总，不包含`results/`原始实验和编译产物。新克隆可以按下面步骤重新生成，不需要下载Kimi K3完整模型权重。

## 1. 环境和构建

需要Linux、C++17编译器、CMake、Python 3及NumPy。报告核验还需要Node.js，图表本身离线运行、不依赖CDN。使用仓库自带ns-3 fork及SUE模块；不能以系统安装的普通ns-3替代。

```bash
git clone --branch hm_emu https://github.com/memory-of-star/SuperNode_Sim.git
cd SuperNode_Sim
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install numpy

# 首次构建底座；已存在且可用时可跳过。
bash simulators/sue/build.sh --target libsue
cmake --build simulators/sue/ns-3-alibabacloud/simulation/build --target libcore libnetwork libpoint-to-point -j8

# 构建本研究的逐包适配器。
bin/sue hm-emu build --config configs/experiments/hm-emu/study-tree.json
```

底座动态库位于`simulators/sue/ns-3-alibabacloud/simulation/build/lib/`，包括`core`、`sue`、`network`、`point-to-point`。适配器构建脚本将该路径写入运行时库搜索路径。编译差异与底座已知限制见`simulators/sue/AGENTS.md`；不要为构建本报告启动无关历史全量测试。

NumPy版本和输入哈希会写入运行记录。严格逐字节复现路由需匹配记录的NumPy版本；同一机器不同输出目录不影响随机种子。每个运行冻结源码和执行二进制，不能在运行期间修改其引用的输入JSON。

## 2. 配置入口

| 用途 | JSON |
|---|---|
| 第二、三章完整矩阵 | `configs/experiments/hm-emu/study-tree.json` |
| 第四章MTP＋公开接受长度 | `configs/experiments/hm-emu/study-mtp-tree.json` |
| 模型结构 | `configs/experiments/hm-emu/sources/hf-config.json` |
| 张量形状、字节与精度 | `configs/experiments/hm-emu/sources/inventory.json` |
| 公开接受长度原始来源 | `configs/experiments/hm-emu/sources/mtp-public/` |

JSON文件引用相对该JSON所在目录解析。当前关键配置：

```json
{
  "modeling": {
    "backend": "sue",
    "topology": "single_switch",
    "collective_algorithm": "tree",
    "branch_schedule": "moe_parallel"
  }
}
```

硬件：每芯片4Die，每Die有效HBM562.5 GB/s；整卡Scale-Up每方向64 GB/s，双向合计128 GB/s；卡内四Die环，每边每方向16 GB/s。每个Die独立接入同一台交换机。端到端固定路径为两侧各850 ns加交换机300 ns预算；其中每侧20 ns链路传播加830 ns XPU接入。SUE额外计入入口序列化、队列、控制帧及重传。时钟1 ps、网络通知1 ns。未改变原AFH、LLR、CBFC代码。

TP8表示8张卡、共32个Die做张量切分，96个注意力头每Die3个。PP各阶段一张卡，内部TP4。B是请求数，初始上下文8192；每条件连续执行两步完整93层decode。没有prefill和真实模型推理，计算/HBM是行为预算。

`execution.workers`是独立仿真进程数，不是单案例多线程。默认24；可按机器资源复制JSON到同目录并调整该值，复制后的配置是新的实验输入，必须保存。输出目录必须不存在。

## 3. 检查并运行主矩阵

```bash
python3 simulators/sue/harness/hm_emu/test_model.py
bin/sue hm-emu check --config configs/experiments/hm-emu/study-tree.json
bin/sue hm-emu run --config configs/experiments/hm-emu/study-tree.json --output results/sue/kimi-tree-new
python3 simulators/sue/harness/hm_emu/audit.py results/sue/kimi-tree-new
```

默认主矩阵576个条件，522个可运行、54个容量拒绝。包括两种端口带宽、全部PP微批划分及B=1/32/256的带宽与固定延时对照。容量拒绝不是性能点。结果可能需要较长墙钟时间，不能用消息级后端或缩短网格填补未完成点。

`run`自动构建并冻结二进制，再生成路由、容量和服务输入。`ledger.jsonl`每完成一个条件追加记录；最终写入`results.json`、`summary.csv`和`verification.json`。只有`failed=0`且所有条件状态齐全才能发布。失败保留原目录，修复后使用新目录，不能覆盖证据。

## 4. 集合通信及资源模型

### 树形AllReduce

卡间采用单棵堆式二叉树，根为rank0，rank r的子节点为2r+1和2r+2，超出卡数者忽略。Reduce向根汇聚，Broadcast沿反向树分发。内部节点只等待自己的子节点，不设置逐轮全体rank同步。广播收到后即可向自己的子节点发送。

每条树边消息包含完整归约向量，均分到四Die端口后由SUE按最多2048 B有效载荷分包。每次AllReduce有`2*(N-1)`条逻辑消息；这是全树消息数，不能乘2 μs当成串行时间。每个子输入在目的卡每Die计入两次读、一次写以及BF16向量加法，服务成本取HBM与加法吞吐约束的较大值。该成本与算子共享本地资源。

当前为整向量树接收后归约，不是slice级流式树，也未实现双树、NVLS或NCCL自动选算法。内部节点最多等待两个子输入；同时在途的MoE归约最多两条分支。消息暂存预算由已有激活缓冲覆盖，未独立模拟DRAM bank和缓存命中。

### 278次调用的依赖

每token仍有93次Attention、1次Dense、92次共享专家、92次路由专家归约。Attention完成后，该层共享/路由分支同时提交工作；每条分支计算完成后独立启动自己的树归约。最后等待两分支汇合，才能继续下一层。latent up保留在下一层依赖段中，不提前执行；最后一层的up在输出尾部完成。

两分支没有被当成各自拥有满速硬件。每卡有一个归一化本地服务池，主模型各依赖段的服务量由算子/HBM/D2D模型给出；多个活跃任务平分服务速率，完成后立即释放份额。树节点向量归约也加入同一池。这样不会凭空增加HBM或计算预算，但对不同资源密集任务的互补重叠较保守：它不是独立SM/HBM/D2D细粒度仲裁。

跨卡阶段仍按卡组结果就绪推进；没有模拟某张卡提前进入下一层。片内四Die归约保留原行为预算，不额外声称片内和卡间流水。PP没有跨卡TP归约，仍按阶段FCFS执行，卡内算子预算不改。

### 证据与测试

新增实现位于`tree_collective.h`和`engine.cc`。`test_tree_collective.cc`验证：本地服务工作量守恒、慢叶节点不阻塞无关子树、归约消息/字节守恒、两个归约可重叠并共享本地服务。主矩阵审计还检查树边数、广播交付、Attention→双分支→下一层依赖。

测试可用与`run.build()`相同的编译参数链接`ns3.36.1-core-default`后执行；下列Python命令从仓库路径自动生成编译命令：

```bash
python3 simulators/sue/harness/hm_emu/test_tree.py
```

## 5. 第四章MTP

```bash
python3 simulators/sue/harness/hm_emu/mtp_run.py --config configs/experiments/hm-emu/study-mtp-tree.json --output results/sue/kimi-mtp-tree-new
python3 simulators/sue/harness/hm_emu/mtp_audit.py results/sue/kimi-mtp-tree-new
```

共13个逐包条件：无MTP基线1个，加上草拟1/2/3/4个token×草拟本地成本倍率0.5/1/2。始终只有1条请求，验证位置为d+1。草拟块和主模型均使用当前树形、分支并行规则。两个候选周期用于计时，不伪称执行了真实token概率计算。

每个位置独立生成top16合成路由，按专家命中次数和32-token tile统计权重读取；被拒绝候选的读取和计算不扣除。同一轮重复专家可复用。计入草拟层代理、特征融合、输出头、验证及保守KDA状态恢复预算。草拟层并非已校准的真实EAGLE权重实现。

公开EAGLE3接受长度来自JSON中的固定值，草拟3个/验证4位置。`cycle_ms / accept_length`是外部接受长度归一化情景估计；外部精度、硬件、工作负载和按请求统计方式与本案例有差别。60%/80%/90%另作假设敏感性。DSpark数据只作背景，不与EAGLE成本混用。

MTP输出速率按`1000*平均提交token数/周期ms`估计；摊销TPOT不代表每个token均匀到达。实际可能等待完整一轮后成批提交。

## 6. 生成、核验和查看报告

```bash
python3 simulators/sue/harness/hm_emu/publish_sue_report.py --run results/sue/kimi-tree-new --mtp-run results/sue/kimi-mtp-tree-new --output docs/reports/hm-emu
```

发布器重新执行两套证据审计，并要求主矩阵TP8 B=1与MTP的无草拟基线一致。然后检查五章编号、SVG、CSV导出、悬停和66种控件组合。该UI检查使用Node DOM桩，不冒充浏览器截图测试。

产物：

- `docs/reports/hm-emu/report.html`：自包含交互报告，可直接浏览器打开。
- `docs/reports/hm-emu/hm-emu-report.zip`：HTML、复现说明、汇总和逐包证据。
- `docs/reports/hm-emu/REPRODUCE.md`：本文的发布副本。

如需本机HTTP查看：

```bash
python3 -m http.server 8000 --bind 127.0.0.1 --directory docs/reports/hm-emu
```

访问`http://127.0.0.1:8000/report.html`。公网隧道是本机运维配置，不是复现实验的前提。发布器不启动公网服务。

## 7. 指标与原始文件

| 文件/指标 | 含义 |
|---|---|
| `experiment.json` | 完整输入快照 |
| `implementation.json`、`source_snapshot/`、`bin/` | 输入/源码/二进制哈希及冻结实现 |
| `routes/`、MTP的`routes.npy` | 合成路由及tile计数 |
| 每条件`service.bin` | 各算子依赖段本地服务预算，单位ps |
| `engine-tokens.csv` | 输出完成事件；TPOT为同一请求两次完成时间差 |
| `engine-execution.csv` | 本地服务与集合通信的开始/结束区间 |
| `engine-sue-ports.csv` | LLR/CBFC/发送计数，阻塞检查数不是等待时间 |
| `engine-sue-wire.csv` | 线上字节和帧数，不等于有效载荷 |
| `engine-sue-profile.json` | 实际端口速率、链路/交换机/接入时延 |
| `audit.json`、`verification.json` | 完整性与独立核验结果 |

第三章把第二步窗口分为“至少一张卡存在本地服务的区间并集”和其补集“无本地服务的暴露等待”；二者相加等于TPOT。集合通信本身可以互相重叠、也可与本地工作重叠，所有调用时长之和不能直接当作应用网络等待。集合通信区间包含树上本地归约，因此本地/通信交集不等于纯链路隐藏时间。

主矩阵output吞吐为`B*2 / 完整运行窗口秒数`，含启动和排空；它与`1000/TPOT`并不严格相同。两步窗口不能当作长时间稳态测速。报告硬件及kernel效率未经真实芯片校准。
