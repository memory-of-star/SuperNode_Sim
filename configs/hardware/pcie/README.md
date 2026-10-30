# Host DDR 与 PCIe 事务模型配置

默认文件 `dual-socket-ddr5-6400.json`：每Host8卡、双路、每路12通道DDR5-6400、Host容量十进制1TB；有效DDR效率0.75。其余延迟和队列参数都是未经实测校准的工程假设，不对应已验证的具体PCIe代际。

从工作区根目录运行：

```bash
bin/sue build
python3 simulators/sue/harness/v2/offload_experiments.py results/sue/pcie-new --smoke \
  --pcie-profile configs/hardware/pcie/dual-socket-ddr5-6400.json
python3 simulators/sue/harness/v2/pcie_report.py results/sue/pcie-new
```

输出目录须不存在。省略`--pcie-profile`仍使用旧的1µs启动＋带宽FIFO。事务模型不叠加旧的`pcie_setup_ns`，使用JSON里的提交间隔与引擎启动延迟。源码接口为`Offload`的`pcie_profile`字典；主资源入口`bin/sue run`目前仍不产生这些卸载任务，应使用上面的offload入口。

参数和机制详见[手册第9章](../../../docs/manual/09-mpu-pcie-offload.md)。所有profile值和请求地址随案例快照保存，编译后的TSV必须与JSON一致，才允许运行。

`access.kv`和`access.engram`可取`sequential/gather/page/host_pack`。默认地址为稳定合成地址，不是真实模型trace。页缓存只对page模式有效，设`page_cache_bytes_per_die=0`关闭；`prefetch_requests_per_descriptor`只控制已就绪DMA描述符内部的有限读前瞻，不进行未知未来token的投机预取。

`link.topology=direct`跳过共享Switch上行；`switch`经过每NUMA共享上行。默认上行512GB/s恰等于4卡×128GB/s，修改为256GB/s或128GB/s可探索超售。`host.remote_numa=true`交换每Host两路DDR归属并计入CPU间互连。

HBM staging与已有HBM任务共用同一资源；`receiver.hbm_bytes_s_per_die`必须匹配硬件profile的每Die有效HBM带宽，否则offload入口拒绝运行。既不隐藏额外HBM写入，也不在事务完成后重复添加整块staging。
