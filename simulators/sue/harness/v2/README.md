# SUE Scale-Up Ethernet 仿真平台 v2

> 当前完整使用与实现手册位于工作区 `docs/simulator/README.md`，单页版为 `docs/simulator/manual.html`。下文早期96项拓扑实验与固定等待描述属于旧工作负载入口；当前资源入口使用硬件JSON、计算/HBM/MPU FIFO和可配置并行策略。参见 `hardware_run.py --help`、`parallel_config.py`。

v2 将当前目录 `topology.md` 的16种拓扑接入逐包 SUE 仿真，增加 Die/MPU 端点、4口组选路、目的侧环转交及原口响应。多Die拓扑入口与通用单口任务入口分别保留，均属于本项目v2。

## 已运行报告

[离线交互报告](../../reports/v2-topology-kimi-glm53/report.html) · [方法和结论](../../reports/v2-topology-kimi-glm53/report.md) · [全部指标](../../reports/v2-topology-kimi-glm53/summary.csv)

16拓扑 × 2模型 × 3窗口的96个实验已执行，完整记录位于上述报告目录的 `cases/`。模型为 Kimi K3、GLM 5.3，使用分别冻结的官方配置；通信是配置推导＋合成路由，不是实测 trace。计算只用固定等待，未模拟完整模型执行。

## 受控探索实验

[交互报告](../../reports/v2-topology-kimi-glm53/exploration-20261008/report.html)覆盖128卡parity/rotate、流量热点、Die亲和放置、UCIe资源、信用窗口、分片、调度和LLR。初始348项加18项固定出口缓冲对照，共366项，363完成、3项512B/parity墙钟超时；失败保留，未计入性能排名。每个条件使用种子1/2/3。

报告采用独立离线看板：拓扑优势热力图可点击联动筛选，调度散点图对照P99与整个完成窗口；数值扫描使用带min–max的曲线，分类条件使用对照柱图。支持按实验、模型、具体条件筛选，导出CSV与各图SVG；完整方法、发现和失败台账在折叠区。图表逻辑单源为`explore_visuals.js`，生成时嵌入HTML，无外部前端依赖。`verify_explore_browser.py`可选使用Playwright核验离线交互、下载和桌面/手机布局。

```bash
python3 harness/v2/explore.py reports/new-exploration 4
python3 harness/v2/explore_credit_control.py reports/new-exploration
# 报告生成依赖：python3 -m pip install markdown-it-py
python3 harness/v2/explore_report.py reports/new-exploration
node harness/v2/verify_explore_ui.js reports/new-exploration/report.html
python3 harness/v2/package_exploration.py reports/new-exploration
```

`workloads.build`增加可选参数`tokens`、`decode_steps`、`compute_ns`、`profile`（uniform/hotspot80/community80）和`placement`（spread/affinity）；默认任务与原矩阵一致。`explore.py`登记条件后执行，追加runner固定交换机出口缓冲以分离信用影响；分析器核对输入哈希、完整交付、拓扑配对和重复控制。详见报告中的方法、逐图说明及适用边界。冻结实验的affinity元数据旧`placement`说明仍为默认公式，实际映射以`die_placement`、任务和路由CSV为准；当前生成器已修正该说明，保留`source/workloads-executed.py`用于追溯。

## 基本使用

从 `simai-scaleup-main` 目录执行：

```bash
python3 harness/v2/run.py build
python3 harness/v2/run.py init harness/v2/examples/my-case \
  --topology xpu128-parity --model kimi-k3 --window decode
python3 harness/v2/run.py validate harness/v2/examples/my-case
python3 harness/v2/run.py run harness/v2/examples/my-case

# 每次使用一个新的矩阵目录，保留旧证据
python3 harness/v2/run.py matrix reports/v2-repeat --workers 2
python3 harness/v2/render.py reports/v2-repeat

# 功能验证，包括预期不可达的负例
python3 harness/v2/verify.py
```

`--workers` 是独立实验进程并行数，不是单次仿真的 MTP 线程数。当前 v2 使用单线程，低层入口外链默认200G，资源入口可按硬件JSON配置链路速率，LLR＋CBFC；通用任务入口的 MTP、PFC/OFF、普通单口拓扑能力保留在 `harness/sue_harness.py`。

单次 `run` 生成原始统计和 `summary.json`；矩阵 `render.py` 生成独立交互HTML、Markdown及汇总CSV。

## 支持的拓扑名称

| 家族 | 名称 |
|---|---|
| 完整轨 | `xpu8`、`xpu16`、`xpu32`、`xpu64` |
| 拆轨 | `xpu128-parity`、`xpu128-rotate`、`xpu256` |
| 混合 | `mixed6-8`、`mixed8-8`、`mixed32-32`、`mixed64-64`、`mixed64-64-pool` |
| 576口 | `xpu18`、`mixed16-8`、`xpu144`、`xpu576` |

布线和容量由 `topologies.py` 生成；纯硬件拓扑图中未使用的备用交换机口不创建网线。`ports.csv` 保留面板口号，`ports.tsv`按面板口排序；内部设备号为每台交换机在用端口的连续序号。R1/R2、物理端口唯一性、SA唯一性和路由键碰撞在执行前检查。

## 自定义任务与参数

Python接口 `run.create(path, topology, tasks, metadata, settings)` 支持通用DAG：

```python
from pathlib import Path
import sys
sys.path.insert(0, 'harness/v2')
from run import create, run_case
from workloads import task
case = create(Path('harness/v2/examples/custom'), 'xpu256', [
    task(0, 0, 255, 2, 5001, delay=1000),
    task(255, 2, 255, 2, 0, delay=5000, deps=[0]),
    task(255, 2, 0, 0, 7003, deps=[1]),
], {'model':'custom','window':'roundtrip'}, {'ring_gbps':400})
print(run_case(case))
```

任务事实来源是 `tasks.json`，`traffic.csv`是可读导出，`tasks.tsv`是编译后输入。任务ID为拓扑序，依赖只可引用较小ID，拒绝重复、自依赖和越界。要改任务请通过 `create` 新建案例；手工改JSON而未同步生成的TSV会被校验拒绝。

每任务有 `src_chip/src_die/dst_chip/dst_die/bytes/delay_ns/vc/group/deps/label`：

- `bytes=0`且源目的SA相同表示本地等待；网络任务不得源目的SA完全相同。
- `group=-1`自动选择一个全组可达的组；`0/1`强制选组。组内实际口由后端确定性哈希决定，不能指定口。VC是独立的流控队列，不是轨号。
- 一个任务固定一个发送口，所有分片同路；不是逐包喷洒，也不是多轨消息条带化。
- 接收端收齐后返回64B事务确认；响应从实际代理Die、请求原口发出。`data_end_ps`是目标Die收齐，`end_ps`是响应回源。后续DAG依赖后者。
- 分片/任务标签为仿真元数据，不占线上字节；生成模型流量中的每记录8B元数据则包含在有效载荷中。

参数集合由 `run.DEFAULT` 限定：种子、分片大小、注入/重试间隔、DAG可见延迟、仿真截止、出口队列容量、UCIe缓冲/速率/每跳时延、错误注入、墙钟超时、CBFC信用。未知参数拒绝。完整 SUE 配置保存在每次快照的 `sue.conf`。

## 路由、代答与UCIe抽象

外部网络使用原SUE模块的设备、链路、交换机、AFH、LLR和CBFC，没有用解析带宽公式代替网络传输。适配器自行按端口构建节点并安装VRF静态路由，避免通用BFS把多端口主机视作中转。

普通档位用 `4*chip+die` 路由键；pure-rail每域只有一种Die，改用chip为10位路由键、MAC低位保留完整chip/Die身份。请求和响应均检查完整地址。576卡配置没有扩大SUE头部，不能用于推断真实芯片也具备同样地址能力。

直接不可达的XPU目标，由同域且离目的Die最近的代理Die承接，同距取最小编号。MPU相关通信必须直接可达，不使用C2C兜底。后端在发送前将没有合法路径的任务标为unreachable、不注入数据帧；负例验证的是拒绝行为，不是实际交换机丢包率。环最短路径，同距取顺时针。UCIe使用每卡共享在途包池及8条独立方向链路；包持有池空间直至到达最终Die，逐跳串行化。池满时保留外部SUE接收信用形成背压。环上仍有有限总在途数据，未声称模拟UCIe PHY/flit/真实信用协议或每链路独立SRAM。

“先交换机后C2C”用于请求；响应在目标Die完成后沿环回到代理，再原口发出。该64B确认仅实现本版的事务结束/原口返回约束，不是完整AXI通道、原子性和内存一致性模型。

## 输出与证据

每个 `runs/<UTC时间-随机ID>/` 保存输入、物理映射、路径预测、命令/哈希、运行日志、退出码/超时、逐任务统计、每端口统计、UCIe边统计及摘要。重复run创建新目录。

- `task_statistics.csv`：请求数据到达、事务响应到达、实际group/port/proxy、UCIe跳数、队列检查和完整性。
- `port_statistics.csv`：端口与面板身份、开始发送的wire字节、已截到仿真结束的发送占用时间、LLR重传/丢弃、CBFC信用检查失败次数。wire字节跨链路重复累计，不是有效载荷。
- `ring_statistics.csv`：方向链路字节/帧、排队时间之和、共享池峰值及准入重试。共享池计数在各边行重复展示，不能对8行重复求和。
- 未完成、路径错误、超时、LLR丢弃都判失败；预期不可达测试仍在仿真结果中标为failed，由测试验收器检查其拒绝行为。

当前没有自动故障口重选、随机传播抖动、实际GPU计算排队、完整模型/调度器、MPU内存服务模型、UCIe PHY或多线程v2验证。网络队列、CBFC、重传及UCIe竞争会产生动态延迟变化。

## 对原模块的改动

唯一受控SUE源码改动在 `network_frontend/sue-config.cc`：`PORT_TO_VRF_MAP`缓存从256项扩为1024项，既有端口语义不变，补足576口档。LLR、CBFC与AFH转发算法未改。新增后端为 `scratch/sue-harness-v3.cc`，通用任务后端保留。`DESIGN.md`原设计不重写，v2新增模型以本说明和运行配置为准。


## 名称与历史证据

产品版本统一为v2，当前命令入口为 `harness/v2/`。旧目录名保留为兼容别名，历史运行日志、后端目标名 `sue-harness-v3` 和输入格式号3保留原值以便核验已有哈希；它们不是另一个产品版本。运行记录未因更名而改写。报告生成需要 `markdown-it-py`（当前环境已安装），缺失时执行 `python3 -m pip install markdown-it-py`。

## 图表与架构图

`features.md`是新增特性文本源，`architecture.svg`是可编辑矢量架构图；render.py将图内嵌到离线HTML并复制到报告目录。`report_charts.py`从任务、端口与token路由记录生成四类补充图的数据。报告共十类数据图，支持筛选、悬停读取数值和SVG下载。

卡间请求量按最多32组聚合；并发图为64个时间桶内的平均未完成网络任务数；端口图为连续16口的整段实验平均发送占用；专家图为路由分配次数。端口逐时带宽、队列时间序列尚未采集。`python3 harness/v2/verify_visuals.py reports/v2-topology-kimi-glm53`检查聚合守恒与原始输入哈希；`node harness/v2/verify_report.js reports/v2-topology-kimi-glm53/report.html`检查全部96个视图的离线交互逻辑。

`topology_diagrams.py`从同一套端口映射生成16张拓扑SVG并内嵌报告，单图在`topologies/`，整包为`topology-diagrams.zip`。图中每列是轨切片，同编号SW在多列出现仍表示同一台交换机的不同VRF。每图含代表卡/MPU接线、全体交换机占用、跨Die直接轨表及UCIe环；验收与已有实验的ports.csv逐项对照。报告另有parity/rotate说明和六组实测对比，完成窗口显示5位小数。

`chart_guide.md`为逐图测量方法与结论的单一文本源。HTML在架构图、拓扑图及十类数据图旁显示可展开说明，Markdown集中列出全部内容。说明中的数字是明确标注的本次固定案例，不随浏览器筛选变化；公共阅读提示强调每个计算等待固定5µs及其扩卡比较边界。
