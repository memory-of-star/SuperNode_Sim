# 7. 使用教学

## 7.1 环境与依赖

当前工作区已经包含完整ns-3 fork源码与可执行文件。日常运行需要Python 3和已有C++可执行文件；重建需要CMake、C++编译器及该工程依赖。代码使用Python 3.10可用的语法，当前环境通过Python 3.10.12执行。不同机器的编译器兼容性要以实际构建日志为准，项目AGENTS中的某些版本说明属于历史环境。

核心实验脚本主要使用Python标准库。生成离线HTML手册需要`markdown-it-py`；可选浏览器验收需要Playwright及Chromium。没有这些可视化工具不会阻止逐包实验运行。模型配置已经冻结在`harness/v2/inputs/`，运行不需要下载完整权重。

## 7.2 查看配置，然后构建

```bash
cd /home/dell/scale_up_evaluation
bin/sue check
bin/sue check --hardware hardware/profiles/current.json \
  --parallel configs/parallel/tp8-global.json --chips 8
bin/sue build
```

`check`打印硬件换算值、解析后的并行参数，以及每个rank所属TP/EP组和专家列表，不执行性能实验。配置错误会在生成新结果前报错。

`build`定向构建`scratch_sue-harness-v3`，使用现有fork。内部开启MTP编译能力不代表实验用多线程；当前case明确配置`SUE_MTP_THREADS=1`。不要为了运行本手册改成编译所有上游test目标：该fork存在已记录的无关测试依赖问题。

## 7.3 第一个实验

```bash
bin/sue run --hardware hardware/profiles/current.json \
  --parallel configs/parallel/tp4-global.json \
  --output results/sue/glm-tp4-example \
  --model glm-5.3 --chips 8 --batch 8 --context 8192 --seed 1 --steps 2
```

含义：8芯片×4Die，共32rank；公共TP4、DP8；默认全局EP32；8条并发序列、8K上下文；连续两个代表块步骤；固定种子1。包含公共计算/访存、TP通信、专家dispatch/compute/combine和输出组内数据交换。

输出目录必须不存在。成功时`results.json`为`passed`；容量不够时为`capacity_rejected`，没有执行逐包仿真。这类拒绝不是程序崩溃，命令可正常退出，因此自动化不能仅检查shell退出码，还必须检查结果状态。

查看单次结果：

```bash
python3 - <<'PY'
import json
from pathlib import Path
root=Path('results/sue/glm-tp4-example')
r=json.loads((root/'results.json').read_text())[0]
print(r['status'],r.get('per_step_block_us'),r.get('moe_dispatch_active_dies'))
print(r.get('run','没有启动逐包运行'))
PY
```

## 7.4 换硬件与并行配置

```bash
cp hardware/profiles/current.json hardware/profiles/my-chip.json
cp configs/parallel/tp4-global.json configs/parallel/my-layout.json
# 编辑两个副本
bin/sue check --hardware hardware/profiles/my-chip.json \
  --parallel configs/parallel/my-layout.json --chips 8
bin/sue run --hardware hardware/profiles/my-chip.json \
  --parallel configs/parallel/my-layout.json \
  --output results/sue/my-chip-my-layout --model glm-5.3 --chips 8
```

CLI中的`--tp`、`--ep`、`--dp`、`--moe-transport`会覆盖并行JSON对应字段；其他字段保留。若JSON显式指定DP8，再用CLI改成TP8而仍使用32rank，会因TP×DP不匹配而拒绝。希望DP自动变化，应让JSON中的`dp`为`null`。

## 7.5 DeepSeek与MPU

```bash
bin/sue run --parallel configs/parallel/tp4-global.json \
  --output results/sue/deepseek-engram-mpu \
  --model deepseek-v4.1-flash-1017 --chips 4 --mpus 2 \
  --engram-remote --batch 8 --context 32768
```

该模式把Engram存放在MPU，专家和其他公共工作仍按模型配置执行。`--engram-remote`只允许DeepSeek且MPU数量大于0。它不表示把模型所有权重或KV全部放在MPU，更不表示完整模型端到端运行。

## 7.6 固定芯片数量比较TP

```bash
for tp in 4 8 16 32; do
  bin/sue run --parallel configs/parallel/tp4-global.json --tp "$tp" \
    --output "results/sue/glm-tp-${tp}-seed1" \
    --model glm-5.3 --chips 8 --batch 8 --context 8192 --seed 1
done
```

JSON的EP为null，解析后仍是32。这样比较公共TP而保留全局专家池。使用相同全局batch；不要把每组batch保持不变而增加总负载后，误称只改变了TP。每个条件至少用多个种子，保留容量拒绝和超时。

## 7.7 预设实验矩阵

```bash
bin/sue run --mode matrix --output results/sue/new-main-matrix --workers 3
bin/sue run --mode restore --output results/sue/new-kv-pages
bin/sue run --mode prefill --output results/sue/new-prefill-chunks
```

主矩阵267条件、冷KV恢复24条件、参数chunk36条件。这些数量描述预设设计，不保证在修改后的硬件上全部容量合格。它们采用各自预设的规模、TP、batch和种子；**并行JSON及TP/EP CLI覆盖仅适用于`--mode case`，传入预设矩阵会明确拒绝**。`--workers`在主矩阵表示独立仿真进程数量，恢复与prefill脚本采用各自既有执行方式，并非通用线程控制。

单条件CLI的`--model/--chips/--batch/...`也不重定义矩阵。需要自定义矩阵时，建议用上述shell循环或编写Python条件列表，再调用`execute()`；不要手改运行后的配置。

## 7.8 原16种拓扑入口与自定义DAG

资源入口固定使用`hw-N-M`八交换机实验拓扑。原始拓扑需要使用低层入口：

```bash
python3 simulators/sue/harness/v2/run.py init results/sue/parity-case \
  --topology xpu128-parity --model kimi-k3 --window decode
python3 simulators/sue/harness/v2/run.py validate results/sue/parity-case
python3 simulators/sue/harness/v2/run.py run results/sue/parity-case
```

这条入口的合成工作负载仍是旧模型，不自动读取并行JSON，也不等同于新资源模型。通用DAG示例：

```python
from pathlib import Path
from workloads import task
from run import create,run_case
rows=[task(0,0,1,0,65536,delay=1000),
      task(1,0,1,0,0,delay=500,deps=[0]),
      task(1,0,0,0,4096,deps=[1])]
case=create(Path('results/sue/custom-dag'),'xpu8',rows,
            {'model':'custom','window':'request-wait-response'})
print(run_case(case))
```

从根目录执行该Python示例时，将`simulators/sue/harness/v2`加入`PYTHONPATH`。网络任务的正字节消息不能同源同目的SA；纯等待必须是同一SA。资源侧车的用法见第3章和`verify_resources.py`。
