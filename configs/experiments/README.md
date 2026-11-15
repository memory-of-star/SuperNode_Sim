# 统一实验JSON

现在单案例、多条件对照和PCIe/MPU卸载都使用同一个入口：

```bash
bin/sue check --config configs/experiments/glm-single.json
bin/sue run --config configs/experiments/glm-single.json --output results/sue/glm-unified
```

`check`展开并校验全部条件，包括模型、并行、资源配置和容量预算，不执行逐包仿真。容量不够会显示`capacity.admitted=false`，不冒充有效性能结果。`run`先做同样的校验，再逐条件调用原有SUE引擎；输出目录必须不存在。首用先执行`bin/sue build`。

## 文件选择

| 文件 | 内容 |
|---|---|
| [glm-single.json](glm-single.json) | GLM、8卡、TP4、IO尽力拼包和HBM公平共享；单条件seed1 |
| [deepseek-16chip-study.json](deepseek-16chip-study.json) | DeepSeek、16卡、TP/MPU部署对照与带宽/效率敏感性；共18条件（不含MPU带宽×2） |
| [glm-kv-offload.json](glm-kv-offload.json) | GLM KV的纯PCIe、纯MPU、固定比例混合卸载；共3条件 |

本目录中的实验JSON均为同一格式，不再需要专用脚本猜测各自结构。旧`deepseek_case_study.py PLAN OUTPUT`识别新格式后转交统一入口；旧格式冻结计划仍保留原处理路径。

## 最小可编辑示例

```json
{
  "schema_version": 1,
  "name": "glm-example",
  "hardware": {"file": "../hardware/profiles/current.json"},
  "model": {
    "config": {"file": "../models/glm-5.3/config.json"},
    "simulation": {"file": "../models/glm-5.3/simulation.json"}
  },
  "parallel": {"file": "../parallel/tp4-global.json"},
  "deployment": {"chips": 8, "mpus": 0},
  "workload": {"batch": 8, "context": 8192, "steps": 2},
  "profiles": {
    "io_die": {"file": "../hardware/io-die/best-effort.json"},
    "hbm": {"file": "../hardware/hbm/fair-share.json"}
  },
  "simulation": {"time_resolution_ns": 0, "timeout_s": 900},
  "seeds": [1],
  "execution": {"workers": 1}
}
```

`file`路径相对于**这份实验JSON所在目录**解析，不随运行命令的当前目录变化；也支持绝对路径。把文件复制到别处时需调整相对引用，或使用运行目录中保存的全内联快照。

## 参数如何组织

| 字段 | 含义与规则 |
|---|---|
| `schema_version`、`name` | 格式版本1；名称为1–80个字母、数字、点、下划线或横线，首位为字母或数字 |
| `hardware` | 原始硬件JSON；可引用或直接内联 |
| `model.config` | Hugging Face config.json；可引用或内联 |
| `model.simulation` | 精度、公共参数量等仿真假设；必须显式指定，不借用其他模型的假设 |
| `parallel` | 并行JSON；省略时TP4，DP/EP按芯片数解析；`dp=null`自动取总Die数/TP，`ep=null`取全局Die数 |
| `deployment` | 必填chips、mpus；当前资源入口支持4/8/16/32卡和0/1/2/4/8/16 MPU，自动生成八轨`hw-N-M`拓扑 |
| `workload` | 必填batch、context、steps；可选engram_remote，默认false |
| `profiles.io_die` | IO Die行为配置；省略或null表示不启用AXI适配器，不等于AXI不拼包 |
| `profiles.hbm` | HBM共享配置；省略或null使用已有默认资源调度，不自动启用公平共享 |
| `profiles.pcie` | 事务级PCIe配置，必须配合offload；不会在普通推理案例中静默忽略 |
| `simulation` | 仿真时序、队列、信用、错误注入与墙钟超时等设置 |
| `offload` | 可选的KV/Engram卸载策略；缺省是普通资源联合推理案例 |
| `seeds` | 唯一的正整数种子数组，缺省[1] |
| `variants` | 对照组数组；名称唯一且不超过64字符，缺省只有base组。每组与种子做笛卡尔展开 |
| `execution.workers` | 同时运行的独立案例进程数，默认1，范围1–64；不是改变单个仿真的硬件并行度 |

`simulation`支持：`payload`、`interval_ns`、`visibility_ns`、`stop_ns`、`queue_bytes`、`ring_bytes`、`ring_delay_ns`、`bad_every`、`timeout_s`、`credits`、`port_sample_ns`、`time_resolution_ns`、`io_event_driven`。链路速率及XPU/MPU接入时延来自hardware，种子来自seeds，避免两个入口定义同一参数。`timeout_s`是程序运行的墙钟上限，`stop_ns`才是仿真时间上限。校验输出展示最终值；当前默认全局精度1ps、请求/依赖间隔1ns。

统一入口不读取`SUE_HARDWARE_JSON`、`SUE_IO_DIE_PROFILE`、`SUE_HBM_PROFILE`的隐式覆盖；所有模型参数以JSON为准。旧式命令仍保留原环境变量行为。不要把旧式`--hardware`等覆盖参数和`--config`混用，统一入口会报错；只允许用`--output`选择结果位置。

## 引用、内联和覆盖

组件可以直接写完整对象，也可以引用并修改少数字段：

```json
"hardware": {
  "file": "../hardware/profiles/current.json",
  "overrides": {"memory_node": {"max_inflight_requests": 256}}
}
```

解析顺序为：**读取组件 → 递归应用组件overrides → 递归应用variant.overrides → 展开种子 → 校验和冻结最终参数**。对象逐字段合并，数组整体替换；不修改被引用文件。variant中的修改只接受内联值，不再接受嵌套file引用。未知字段、重复JSON键、非法比例及不兼容参数都会报错。

例如在基准配置中添加：

```json
"variants": [
  {"name": "tp4"},
  {"name": "tp8", "overrides": {"parallel": {"tp": 8, "dp": null}}},
  {"name": "no-packing", "seeds": [1],
   "overrides": {"profiles": {"io_die": {"mode": "none"}}}}
],
"seeds": [1, 2, 3]
```

会运行3＋3＋1＝7个条件。variant.seeds替换全局种子数组。变更TP时，若基础配置固定了DP，须同步修改DP或设null；不会悄悄忽略用户给定的DP。

提高hardware中的HBM有效带宽时，也应同步修改HBM profile及（若启用）PCIe接收侧带宽；两者不一致会在仿真前被拒绝。DeepSeek示例的`hbm-2x`展示了这种配套修改。

## PCIe与MPU卸载

在同一个JSON里增加profiles.pcie和offload即可：

```json
"offload": {
  "target": "kv",
  "backend": "hybrid",
  "mpu_fraction": 0.25,
  "chunk_bytes": 262144
}
```

- `target`：kv、engram或both；Engram仅支持DeepSeek。
- `backend`：pcie、mpu、hybrid或tiered。pcie要求mpus=0；其余要求mpus>0。tiered要求target=both，表示Engram放MPU、KV走PCIe。
- hybrid必须显式给定0–1的mpu_fraction；不隐式推导nominal share。其他backend不能填写该比例。
- `chunk_bytes`默认262144，控制任务层的分块，与256 B AXI事务、MRRS/MPS不是同一概念。
- 卸载案例须显式提供PCIe profile，包括纯MPU对照，以固定Host容量核算和对照配置。MPU-only不会因此产生PCIe数据流。
- 不与workload.engram_remote=true混用，数据放置统一由offload控制。

新入口尊重hardware中的MPU效率。当前默认硬件明确设置`hardware.assumptions.mpu_efficiency=1.0`，每MPU单向有效带宽为190 GB/s；修改效率会改变服务带宽，需重新生成并运行案例。

## 输出与复现

输出根目录保存experiment.json、expanded.json、sources.json（原始输入路径及SHA256）、validation.json、implementation.json、results.json、summary.csv和verification.json。每个条件位于`conditions/<variant>-seed<N>/`，包含：

- `experiment.json`：无外部文件引用的完整有效输入，默认并行/时序值也已展开，可直接再次传给`bin/sue check/run --config`。
- `hardware-profile.json`、`hardware.json`、模型输入、`parallel.json`、`definition.json`：实际输入及换算结果。
- `cases/`及其run目录：原有SUE任务、资源、端口、AXI、PCIe统计和校验结果。
- `result.json`、`stdout.log`、`stderr.log`：案例结果与失败线索。

结果中的capacity_rejected是容量拒绝，不是成功性能点；其他运行失败记录在台账并让整个命令返回非零。条件间使用独立进程，防止硬件全局参数或IO环境变量相互污染。已有结果目录不会被覆盖。

这些案例仍是代表块/阶段仿真，不是完整模型TPOT/TTFT。统一输入没有扩展任意拓扑、PP、多种训练算法等底层能力，也不会自动重跑或改写手册中的历史实验。

## hm_emu 专用完整 decode 研究

`hm-emu/study.json` 使用单独的 `hm-emu-1` schema，经 `bin/sue hm-emu check/run --config ...` 执行。该扩展默认执行 Kimi K3 TP/PP 与单层交换机的 SUE 逐包实验；消息级参考需显式选择 study-message.json，不能交给默认的代表块入口。硬件、工作负载、tile假设及敏感性均在同一 JSON 中；详见 [扩展文档](../../simulators/sue/harness/hm_emu/README.md)。
