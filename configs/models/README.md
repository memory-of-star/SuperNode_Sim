# 模型 JSON 配置

当前资源建模入口 `bin/sue check/run` 支持 Kimi K3、GLM-5.3、DeepSeek V4.1 Flash 1017 三个架构适配器。目录名是兼容模型标识；修改目录内 JSON 后无需修改 Python 或重新编译 C++。

每个模型包含：

- `config.json`：原样保存的 Hugging Face 模型配置，支持外层 `text_config`。现有快照直接迁移自此前实验冻结的文件，没有静默升级上游版本。
- `simulation.json`：仿真所需、HF 配置没有充分描述的参数与精度假设。
- `source.json`：下载来源、revision/commit、原始 SHA256。修改配置后源哈希不再匹配，运行记录会明确标记；不再将其视作原始官方文件。

## 使用

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

## 复用 Hugging Face 配置

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

## 字段如何影响仿真

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

## simulation.json 字段

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

## 校验与范围

检查必需字段、正整数/有限数、Top-K≤专家数、MoE层数、层列表、压缩比例、家族一致性、TP整除和补充文件未知字段。Kimi层号按官方1起算，DeepSeek源层按0起算；DeepSeek额外MTP层的压缩列表可保留，但不执行MTP。

HF中其他字段为兼容原始文件而保留，未使用字段不会全部报错。视觉编码器、DSpark/MTP、真实路由logits、具体激活函数、RoPE算术、量化kernel和所有逐张量权重没有据此执行。当前GLM索引调度仍是按频率的简化模型；不是完全解释每个HF算子。网络协议仍是SUE，模型JSON只改变上层任务与资源预算。

旧 `harness/v2/run.py init` 拓扑固定等待入口保留其冻结输入，尚不支持这组 CLI 参数。新增可配置模型入口是 `bin/sue check/run`，不要将两种入口混淆。

## 证据与复现

输出根目录和实际逐包运行目录包含：`model-config.json`、`model-simulation.json`、`model-source.json`。设计文件还嵌入完整输入，避免并发运行依赖全局可变模型。`command-manifest.json` 记录运行快照哈希；source中记录读取时原文件字节哈希。快照JSON会规范缩进，故其字节哈希可能不同于原始下载文件，数据内容一致。

复用运行快照：

```bash
bin/sue check --model-config results/sue/my-model-json-run/model-config.json \
  --model-simulation results/sue/my-model-json-run/model-simulation.json
```

验证入口：`python3 simulators/sue/harness/v2/verify_models.py results/sue/new-model-validation`。它检查三种模型的名称/JSON入口等价、改参行为、非法配置、逐包完成和快照哈希。结果属于代表块通信/资源仿真，不是完整模型推理测速。
