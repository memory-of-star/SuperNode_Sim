# 2. 硬件、并行、实验与网络配置

## 2.1 配置分成四类

| 配置 | 文件/入口 | 作用 |
|---|---|---|
| 模型结构与仿真假设 | `configs/models/*/{config,simulation}.json`，`--model-config/--model-simulation` | HF结构、精度与公共参数假设 |
| 物理规格与资源效率 | `hardware/profiles/*.json`，`--hardware` | 算力、容量、HBM/D2D/SUE/MPU带宽和有效效率 |
| 并行布局 | `configs/parallel/*.json`，`--parallel` | TP/DP/EP、专家和token放置、MoE收发方式 |
| 工作负载条件 | CLI或Python定义 | 模型、芯片数、MPU数、batch、上下文、步骤、种子 |
| 网络与调度细节 | `SETTINGS`、`run.create(settings=...)`、生成的`sue.conf` | 分片、队列、信用、错误注入、仿真截止等 |

优先级：内置默认并行配置 → 并行JSON → 显式并行CLI覆盖。硬件从所选JSON加载；预设敏感性实验可通过`hardware_overrides`覆盖效率/延迟，最终case元数据记录实际值。修改运行目录里的`sue.conf`不是修改全局模板的正确方式：会破坏输入校验和复现关系。

## 2.2 硬件JSON完整字段

原始样例在`hardware/profiles/current.json`。数值均采用十进制SI单位，TB/s=10^12 byte/s，Gbit/s=10^9 bit/s。GB与GiB不同，恢复实验中的MiB按2^20 byte计算。

| 字段 | 当前值 | 范围、换算与行为 |
|---|---:|---|
| schema_version | 1 | 只接受当前格式1 |
| name | current-4die | 配置名称，不参与性能计算 |
| chip.dies | 4 | 当前结构固定4，不支持改成8 |
| chip.fp4_tflops_per_die | 900 | 每Die峰值FP4算力 |
| chip.fp8_tflops_per_die | 450 | 每Die峰值FP8算力 |
| chip.fp16_tflops_per_die | 225 | 每Die峰值FP16算力 |
| chip.memory_capacity_bytes | 144000000000 | 每芯片144GB，均分36GB/Die |
| chip.memory_bandwidth_bytes_s | 15000000000000 | 每芯片15TB/s，均分3.75TB/s/Die |
| chip.d2d_one_way_bytes_s | 2000000000000 | 芯片单向总发送预算2TB/s；双向可合计4TB/s |
| chip.scaleup_bidirectional_bytes_s | 1600000000000 | 芯片双向1.6TB/s；单向0.8TB/s |
| chip.scaleup_ports_per_die | 8 | 固定8，改变则拒绝 |
| chip.pcie_bytes_s | 128000000000 | 芯片128GB/s，仅记录，没有PCIe传输事件 |
| memory_node.capacity_bytes | 512000000000 | 每MPU512GB |
| memory_node.memory_bandwidth_bytes_s | 190000000000 | 每MPU190GB/s峰值内存服务率 |
| memory_node.scaleup_ports | 8 | 固定8 |
| memory_node.port_bits_s | 200000000000 | MPU单端口200Gbit/s，可独立于XPU修改 |
| assumptions.compute_efficiency | 0.30 | 峰值计算乘此效率；范围(0,1] |
| assumptions.hbm_efficiency | 0.50 | 本地内存有效带宽比例 |
| assumptions.mpu_efficiency | 0.50 | MPU内存有效带宽比例 |
| assumptions.mpu_fixed_ns | 200 | 每次服务固定延迟，非负整数ns |
| assumptions.runtime_reserve_fraction | 0.20 | 可分配容量按1−比例计算，范围[0,1) |
| assumptions.kv_element_bytes | 2 | 当前KV公式只支持2；不是任意精度开关 |

效率、固定延迟和容量预留属于建模假设。它们不能由“峰值规格已知”自动推出，也未通过当前实验自我校准。

换算公式：

```text
每Die HBM带宽 = 芯片HBM带宽 / 4
每Die FP8有效算力 = fp8_tflops_per_die × 10^12 × compute_efficiency
XPU单端口bit/s = scaleup_bidirectional_bytes_s / 2 / 4 / 8 × 8
环单有向边byte/s = d2d_one_way_bytes_s / 4 / 2
MPU总接入单向byte/s = scaleup_ports × port_bits_s / 8
```

当前XPU端口200Gbit/s、每Die单向200GB/s；环有8条有向边，每条250GB/s=2000Gbit/s。环总预算是对用户给定D2D带宽的一种分配假设，不是对某个真实UCIe PHY lane配置的复现。

修改HBM带宽会影响资源任务服务时间，修改scale-up会影响C++实际NetDevice速率，修改D2D会影响环序列化时间，修改容量会影响运行前拒绝。PCIe当前只有记录作用，不能用于得出PCIe offload结果。

## 2.3 并行JSON

```json
{
  "schema_version": 1,
  "name": "tp4-ep-global",
  "tp": 4,
  "dp": null,
  "ep": null,
  "expert_tp": 1,
  "pp": 1,
  "moe_transport": "tp-striped",
  "expert_placement": "round_robin",
  "token_assignment": "round_robin",
  "decode_expert_reuse_probability": 0.75
}
```

| 字段 | 默认值 | 实际含义 |
|---|---|---|
| tp | 4 | 公共部分每个TP组的rank数量 |
| dp | null | 自动计算`world_size/tp`；显式值必须满足TP×DP=world_size |
| ep | null | 自动使用全网rank；显式值是每个专家组的rank数量 |
| expert_tp | 1 | 专家内部张量并行，目前只接受1 |
| pp | 1 | 流水线阶段数，目前只接受1 |
| moe_transport | tp-striped | `tp-striped`按特征切片多Die收发；`leader`为旧集中对照 |
| expert_placement | round_robin | 专家在每个EP组内按余数分配；`contiguous`按连续编号块分配 |
| token_assignment | round_robin | token轮转分配到TP组；`contiguous`平衡地分配连续token区间 |
| decode_expert_reuse_probability | 0.75 | 后续步骤沿用该token上一步完整专家集合的概率 |

未提供的字段采用默认值；未知字段、非法枚举和不支持的并行结构报错。`null`只有DP/EP接受，含义是自动推导，不是0。

## 2.4 TP/DP/EP怎样划分

`world_size = chips × 4`，`rank = chip × 4 + die`。当前使用连续rank分组，不支持任意自定义rank列表或拓扑感知自动优化。

```text
TP组g = [g×TP, ..., (g+1)×TP−1]
EP组h = [h×EP, ..., (h+1)×EP−1]
公共部分DP组数量 = world_size / TP
专家副本组数量 = world_size / EP
```

约束：TP整除world_size；EP整除world_size；EP必须是TP的整数倍，因此每个TP组完整落在一个EP组中。不同EP组各保存完整的一套路由专家，**EP不是越小越节省内存**。

以8芯片32rank为例：

| 配置 | 公共TP组 | 公共DP | 专家池 |
|---|---|---:|---|
| TP4、EP32 | 每芯片一个TP组 | 8 | 全网一套专家 |
| TP8、EP32 | 每两芯片一个TP组 | 4 | 全网一套专家 |
| TP8、EP16 | 每两芯片一个TP组 | 4 | 前后各16rank形成两套专家副本 |
| TP32、EP32 | 所有芯片一个TP组 | 1 | 全网一套专家 |

TP×EP不等于设备总数，它们用于不同部分。公共层在TP组内切分参数，路由专家由EP组按专家分配。Attention不按“专家编号”切EP；一个专家的矩阵也没有自动再切成TP份。

GLM每个MoE层256个路由专家：EP32时每rank8个，EP16时每rank16个。原36GB/Die规格下，GLM的某些EP16部署会因专家副本占用被拒绝。`tp8-ep16.json`展示合法布局，不承诺原硬件一定装得下。

## 2.5 专家位置与token归属

`round_robin`专家放置：

```text
EP组起点 = floor(TP组起始rank / EP) × EP
专家owner = EP组起点 + expert_id % EP
```

`contiguous`专家放置：组内每rank一个连续块，块宽`ceil(专家数/EP)`，最后一个块可较小。每个EP组覆盖全体专家，可能有空rank；不声称完美负载均衡。

token轮转为`token_id % DP`。连续分配把batch分成DP个区间，前`batch % DP`个区间多1个token；当batch小于DP时，只有前batch个TP组活跃。

返回结果回到请求所属TP组，不会进入另一个EP副本组。结果文件`parallel-layout.json`列出全部组与每rank专家编号，便于检查实际放置，不能只看“TP8/EP16”标签猜测映射。

## 2.6 实验CLI字段

| 选项 | 默认 | 说明 |
|---|---|---|
| --hardware | `bin/sue`自动提供current.json | 物理规格文件 |
| --parallel | 无文件，采用默认策略 | JSON策略 |
| --tp / --dp / --ep | 不覆盖JSON | 显式覆盖相应字段 |
| --moe-transport | 不覆盖JSON | `tp-striped`或`leader` |
| --model | glm-5.3 | 另支持kimi-k3、deepseek-v4.1-flash-1017 |
| --chips | 8 | 资源入口支持4/8/16/32 |
| --mpus | 0 | 支持0/2/4/8 |
| --batch | 8 | 代表decode步骤的全局并发序列数 |
| --context | 8192 | 每序列上下文token数，影响KV预算与访存 |
| --steps | 2 | 连续代表块步骤数，不是完整模型层数 |
| --seed | 1 | 合成专家选择和端口哈希种子 |
| --engram-remote | 关闭 | DeepSeek Engram放MPU |
| --mode | case | case/matrix/restore/prefill |
| --workers | 3 | 主矩阵外层进程并发数 |
| --output | 必填 | 新结果目录，拒绝覆盖已有目录 |
| --validate-only | 关闭 | 只解析配置，不生成性能结果 |

## 2.7 网络与调度配置

资源实验的有效设置由`run.DEFAULT`、`inference_experiments.SETTINGS`、硬件换算和seed组合：

| 设置 | 资源实验默认 | 行为 |
|---|---:|---|
| payload | 8192 byte | 应用消息的分片载荷上限，非完整线上帧长 |
| interval_ns | 10 | 发下一片/出口阻塞重试间隔 |
| visibility_ns | 20 | 父任务完成到子任务可见的通知延迟 |
| stop_ns | 100000000 | 仿真截止100ms；不是墙钟超时 |
| timeout_s | 90 | 单进程墙钟超时 |
| queue_bytes | 262144 | 主机出口每VC队列设置，不等于交换机总缓冲 |
| ring_bytes | 1048576 | 每芯片环共享池，不是每条边各1MiB |
| ring_gbps | 由硬件推导，当前2000 | 每条有向环边Gbit/s |
| ring_delay_ns | 20 | 环每跳固定延迟 |
| credits | 512 | 每VC配置的信用额度；不能直接当成byte |
| bad_every | 0 | 关闭确定性错误注入；非零用于LLR实验 |
| xpu_port_bps / mpu_port_bps | 硬件推导 | 每端口bit/s |

生成的`sue.conf`进一步固定链路传播20ns、交换机转发预算300ns、CBFC模式、单线程、端口VRF映射等。当前没有通用CLI把这些全变成任意JSON；高级实验通过`run.create(...,settings=...)`定义并保留快照。交换机预算不是全路径总时延，还要加序列化、排队、控制流和依赖通知。


# 模型 JSON 配置与 Hugging Face 导入

当前资源建模入口 `bin/sue check/run` 支持 Kimi K3、GLM-5.3、DeepSeek V4.1 Flash 1017 三个架构适配器。目录名是兼容模型标识；修改目录内 JSON 后无需修改 Python 或重新编译 C++。

每个模型包含：

- `config.json`：原样保存的 Hugging Face 模型配置，支持外层 `text_config`。现有快照直接迁移自此前实验冻结的文件，没有静默升级上游版本。
- `simulation.json`：仿真所需、HF 配置没有充分描述的参数与精度假设。
- `source.json`：下载来源、revision/commit、原始 SHA256。修改配置后源哈希不再匹配，运行记录会明确标记；不再将其视作原始官方文件。

### 使用

从工作区根执行：

```bash
bin/sue check --model-config configs/models/glm-5.3/config.json
bin/sue run --model-config configs/models/glm-5.3/config.json \
  --hardware hardware/profiles/current.json \
  --parallel configs/parallel/tp4-global.json \
  --chips 8 --batch 8 --context 8192 \
  --output results/sue/my-model-json-run
```

`--model-config` 自动读取同目录的 `simulation.json`。文件位于别处时明确指定：

```bash
bin/sue check --model-config /path/to/config.json \
  --model-simulation /path/to/simulation.json
```

也保留 `--model glm-5.3` 等名称入口：读取这里的 JSON。两种参数同时使用时必须属于同一模型家族；未指定时默认 GLM。`--model-config`、`--model-simulation`、`--model` 的显式覆盖仅适用于单条件 `case`，预置矩阵仍按自己的模型列表运行，拒绝忽略覆盖参数。

推荐复制整个模型目录做敏感性实验，避免改变默认基线。路径可任意命名，家族由 HF `model_type` 确定。不会因为文件名写成 GLM 就接受另一种架构。

### 复用 Hugging Face 配置

当前冻结来源见各目录 `source.json`；上游配置页：

- [Kimi K3](https://huggingface.co/moonshotai/Kimi-K3/blob/main/config.json)
- [GLM-5.3](https://huggingface.co/zai-org/GLM-5.3/blob/main/config.json)
- [DeepSeek V4.1 Flash 1017](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017/blob/main/config.json)

下载对应仓库的根目录 `config.json` 即可，不是 `generation_config.json`，也不是 DeepSeek 的 `inference/config.json`。无需下载权重、安装 Transformers、执行 `auto_map` 指定的远程 Python。此入口只解析 JSON，不自动联网。

推荐使用固定 commit 的下载链接，例如：

```bash
curl -fL 'https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017/resolve/1bae0e1088993b9b82de08f59de5be5a34e42170/config.json' -o /tmp/deepseek-config.json
bin/sue check --model-config /tmp/deepseek-config.json \
  --model-simulation configs/models/deepseek-v4.1-flash-1017/simulation.json
```

原始 HF 配置没有公共层活跃矩阵参数量、实际部署计算精度等全部信息。外部配置缺少同目录 `simulation.json` 时必须明确传入补充文件，不会静默套用另一模型的数值。修改结构后应复核补充假设；这里支持这三个架构的配置驱动敏感性实验，不代表支持其家族所有历史或未来版本。

### 字段如何影响仿真

| HF 字段 | 实际用途 |
|---|---|
| `num_hidden_layers`、`first_k_dense_replace` | 层数与连续 MoE 层数；容量及公共计算/访存均摊 |
| `hidden_size` | TP 公共激活/归约消息宽度 |
| `routed_expert_hidden_size`（可选） | 专家输入宽度；未提供时等于 hidden_size |
| `moe_intermediate_size` | 每专家参数量 `3 × 专家输入宽度 × 中间维度` |
| `num_experts` / `n_routed_experts` | 专家数量、EP 放置、容量及路由采样 |
| `num_experts_per_token` / `num_experts_per_tok` | 每 token 路由专家数与 dispatch/combine 流量 |
| `vocab_size`、`tie_word_embeddings` | 输入 embedding 与输出 head 的容量预留；绑定时只计一份 |
| Kimi `linear_attn_config`、`kv_lora_rank`、`qk_rope_head_dim` | MLA KV 缓存与 KDA 矩阵状态的容量/访存预算 |
| GLM `index_topk`、`index_topk_freq`、`index_head_dim`、MLA 维度 | 稀疏注意力缓存及简化索引扫描预算 |
| DeepSeek `compress_ratios`、KV/index 源层、`sliding_window`、head 维度、`index_topk` | 压缩缓存、窗口和索引扫描预算 |
| DeepSeek `engram_num_embeddings`、`engram_head_dim` | Engram 存储容量 |
| DeepSeek `engram_layer_ids`、`engram_max_ngram_size`、`engram_n_heads` | 每 token 查表行数；本地 HBM 或远程 MPU 服务及 SUE 回传字节 |

Top-K 不同于 HF 生成采样参数 `top_k`；后者不控制 MoE 专家路由。

### simulation.json 字段

| 字段 | 单位/作用 |
|---|---|
| `schema_version` | 当前必须是整数 1 |
| `family` | 必须与 HF model_type 对应，三个兼容模型标识之一 |
| `common_active_params` | 公共部分活跃矩阵参数个数，不含路由专家和单列 embedding 预留；用于公共计算/访存/容量 |
| `expert_weight_bytes`、`common_weight_bytes` | 每参数平均存储字节，含假定量化开销 |
| `expert_dtype`、`common_dtype` | `fp4/fp8/fp16`，选择硬件 JSON 中相应计算吞吐 |
| `base_chips` | 预置实验矩阵的起始芯片数；单次运行由 `--chips` 决定 |
| `engram_storage_bytes_per_element` | Engram 每元素存储字节，默认1.03125 |
| `engram_lookup_bytes_per_element` | 每次查表返回的元素字节，默认1 |
| `engram_lookup_metadata_bytes` | 每行返回的量化/元数据字节，默认8 |
| `notes` | 假设来源与局限说明 |

默认公共参数来自此前按圆整激活参数量反推的估算：`104B/39B/16B − MoE层数 × Top-K × 单专家参数数`。迁移后将反推结果显式写入 JSON，避免改 Top-K 时公共层参数反向自动增加。需要更准确的公共算子模型时应替换该假设。

计算精度与存储精度是两个独立字段。例如只把 `expert_dtype` 从 fp8 改成 fp4，只改变计算速率；如果也想改变权重字节数，必须同步修改 `expert_weight_bytes`。HF `dtype/quantization_config` 原样保留，但不会自动覆盖显式仿真精度。embedding 预留仍为每参数2B，激活/KV仍按既有模型取2B、KDA状态4B；这些尚未成为此 JSON 的可调字段。

### 校验与范围

检查必需字段、正整数/有限数、Top-K≤专家数、MoE层数、层列表、压缩比例、家族一致性、TP整除和补充文件未知字段。Kimi层号按官方1起算，DeepSeek源层按0起算；DeepSeek额外MTP层的压缩列表可保留，但不执行MTP。

HF中其他字段为兼容原始文件而保留，未使用字段不会全部报错。视觉编码器、DSpark/MTP、真实路由logits、具体激活函数、RoPE算术、量化kernel和所有逐张量权重没有据此执行。当前GLM索引调度仍是按频率的简化模型；不是完全解释每个HF算子。网络协议仍是SUE，模型JSON只改变上层任务与资源预算。

旧 `harness/v2/run.py init` 拓扑固定等待入口保留其冻结输入，尚不支持这组 CLI 参数。新增可配置模型入口是 `bin/sue check/run`，不要将两种入口混淆。

### 证据与复现

输出根目录和实际逐包运行目录包含：`model-config.json`、`model-simulation.json`、`model-source.json`。设计文件还嵌入完整输入，避免并发运行依赖全局可变模型。`command-manifest.json` 记录运行快照哈希；source中记录读取时原文件字节哈希。快照JSON会规范缩进，故其字节哈希可能不同于原始下载文件，数据内容一致。

复用运行快照：

```bash
bin/sue check --model-config results/sue/my-model-json-run/model-config.json \
  --model-simulation results/sue/my-model-json-run/model-simulation.json
```

验证入口：`python3 simulators/sue/harness/v2/verify_models.py results/sue/new-model-validation`。它检查三种模型的名称/JSON入口等价、改参行为、非法配置、逐包完成和快照哈希。结果属于代表块通信/资源仿真，不是完整模型推理测速。
