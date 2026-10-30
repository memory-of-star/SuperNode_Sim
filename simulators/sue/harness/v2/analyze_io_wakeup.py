"""Report observed IO-scheduler speedups separately from frozen experiment curves."""
import argparse, json
from pathlib import Path
from run import csvread, dump, sha

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('study',type=Path);ap.add_argument('--docs',type=Path,required=True)
    a=ap.parse_args();root=a.study.resolve();dest=a.docs.resolve();dest.mkdir(parents=True,exist_ok=True)
    checks=json.loads((root/'verification/verification.json').read_text())
    io=json.loads((root/'functional-io/verification.json').read_text())
    mem=json.loads((root/'functional-memory/verification.json').read_text())
    legacy=json.loads((root/'legacy-verification.json').read_text())
    assert all(r['status']=='passed' for r in (checks,io,mem,legacy))
    rows=[]
    for r in sorted(json.loads((root/'benchmark/results.json').read_text()),key=lambda r:r['case_index']):
        assert r['status']=='passed'
        new=Path(r['run']);old=Path(r['baseline_run'])
        for name in ('tasks.json','resources.json','token_routes.csv','io-die.json','io-mpu.tsv','pcie.json','hbm.json'):
            assert sha(new/name)==sha(old/name),(r['case_index'],name)
        settings=json.loads((new/'experiment.json').read_text())['settings']
        assert settings['interval_ns']==settings['visibility_ns']==settings['time_resolution_ns']==settings['io_event_driven']==1
        profile=json.loads((new/'io-die.json').read_text());assert profile['profile']['issue_interval_ns']==1
        assert profile['axi_mpu']['max_inflight']==128
        row=dict(seed=settings['seed'],new_run=str(new),old_run=str(old),
            new_wall_s=r['wall_s'],new_total_with_validation_s=r['total_with_validation_s'],
            new_block_us=r['block_us'],new_blocked=r['blocked_attempts'],scheduler=r['scheduler'])
        if (old/'execution.json').exists() and (old/'task_statistics.csv').exists():
            execution=json.loads((old/'execution.json').read_text());assert execution['exit_code']==0
            before=csvread(old/'task_statistics.csv');after=csvread(new/'task_statistics.csv');assert len(before)==len(after)
            for x,y in zip(before,after):
                for field in ('id','bytes','received_bytes','port','proxy_die','ucie_hops','status'):assert x[field]==y[field],field
            old_us=(max(int(x['end_ps']) for x in before)-1000000)/2e6
            old_blocked=sum(int(x['queue_blocked_polls']) for x in before)
            row.update(old_wall_s=execution['wall_s'],wall_speedup=execution['wall_s']/r['wall_s'],
                old_block_us=old_us,block_change_pct=100*(r['block_us']/old_us-1),old_blocked=old_blocked,
                max_task_end_delta_ns=max(abs(int(x['end_ps'])-int(y['end_ps'])) for x,y in zip(before,after))/1000)
            for label,path in (('old',old),('new',new)):
                port=csvread(path/'port_statistics.csv')
                row[label+'_replays']=sum(int(x['replay_frames']) for x in port)
                row[label+'_wire_bytes']=sum(int(x['wire_bytes']) for x in port)
        rows.append(row)
    data=dict(status='passed',scope='Three full Kimi/1MPU cases; paired inputs. Baseline pending if no execution record.',
        functional_runs=checks['runs']+io['cases']+mem['cases'],legacy_comparisons=len(legacy['cases']),
        benchmark_cases=len(rows),rows=rows,small_cases=checks['comparisons'])
    dump(dest/'io-wakeup-data.json',data)
    lines=['# IO背压事件唤醒：速度与时序验证','',
        '保留1 ns请求产生/重试时间网格、1 ns依赖通知和1 ns MPU服务窗口，消除队列满时逐请求的周期轮询。没有改成10 ns，也没有修改AFH、LLR、CBFC协议算法。', '',
        '本节独立记录调度优化，不替换第8、9章的旧轮询冻结数据。全局时间精度也未因优化改变：完整Kimi配对使用原1 ns网格，小案例分别检查1 ns和1 ps。', '',
        '## 如何加速','',
        'AXI记录队列满后，将请求登记为尚未被接纳的等待者；同一端口统一管理其原重试时钟相位。队列释放空间时，才在可行的原重试时间点恢复请求。同相位等待者按登记顺序处理。等待列表替代原先挂在事件队列中的重试回调，不增加硬件队列容量。', '',
        '设备出口满时，接入端点设备已有的EgressFreedHook，释放空间后恢复该端口发送；不覆盖交换机的背压回调。保留发送机会间隔、有限队列、信用约束及内存FIFO。', '',
        '## 完整Kimi案例','',
        '16 XPU、1 MPU、TP32、EP64、B64、上下文32768、两步代表块、128条MPU在途请求，IO尽力拼包。任务、资源、专家路由、IO配置、PCIe和HBM输入均与旧案例核对哈希相同。比较的是调度实现，绝不把代表块耗时称为模型TPOT。', '',
        '| 种子 | 旧仿真进程秒 | 事件唤醒秒 | 执行加速倍数 | 旧代表块µs | 新代表块µs | 代表块变化 |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in rows:
        if 'old_wall_s' in r:lines.append(f"| {r['seed']} | {r['old_wall_s']:.3f} | {r['new_wall_s']:.3f} | {r['wall_speedup']:.2f}× | {r['old_block_us']:.4f} | {r['new_block_us']:.4f} | {r['block_change_pct']:+.4f}% |")
        else:lines.append(f"| {r['seed']} | 原运行尚未结束 | {r['new_wall_s']:.3f} | — | — | {r['new_block_us']:.4f} | — |")
    lines+=['','墙钟时间来自execution.json，包含仿真进程启动和CSV输出，不含Python逐记录校验及后续无损压缩。三个新案例从启动到校验完成约为'+
        '、'.join(f"seed{r['seed']} {r['new_total_with_validation_s']:.1f}秒" for r in rows)+
        '。每个条件只测了一次墙钟，且有其他并发工作，执行加速倍数是本机观测值，不是跨机器保证。', '',
        '| 种子 | 旧阻塞尝试 | 新阻塞尝试 | 等待注册数 | 端口唤醒事件 |', '| --- | ---: | ---: | ---: | ---: |']
    for r in rows:lines.append(f"| {r['seed']} | {r.get('old_blocked','尚未完成')} | {r['new_blocked']:,} | {r['scheduler']['parked']:,} | {r['scheduler']['wake_events']:,} |")
    lines+=['','阻塞尝试是适配器的失败入队/出口检查，既不是SUE重传包数，也不是阻塞持续时间。事件模式减少的是无效执行事件；不能据其数量下降宣称硬件拥塞降低。', '',
        '## 正确性与边界','',
        f"20组调度配对、12组IO功能、7组AXI内存回归均通过；另有{len(legacy['cases'])}组旧二进制对照，任务/端口/资源/环及IO记录、包和线上轨迹共7类CSV逐字节一致，证明io_event_driven=0保留了这些案例的旧轮询行为。三个完整Kimi案例也通过了原有的逐请求和资源校验。", '',
        '验证覆盖拼包与不拼包、小队列、多目的/多VC、读写混合、MPU返回、跨Die代理、错误重放、尾部不足256 B、在途上限、每周期FIFO带宽、HBM共享及PCIe交付。全部等待者最终恢复，pending=0，parked=resumed。', '',
        '事件模式不是逐事件等价：去掉轮询会改变同时间戳的事件插入顺序，影响抢占空位、拼包及个别任务完成时间。小案例的整体结束时刻一致，但个别任务可存在微秒级差异；完整Kimi的偏差见上表。不能仅凭总窗口接近就声称每个任务时序相同，也没有据此重新解释第9章原曲线。', '',
        '## 配置和复验','',
        '当前适配器默认事件唤醒；在案例experiment.json的settings中显式设置`"io_event_driven": 1`，旧轮询对照设为0。两者都保留`interval_ns=1`、`visibility_ns=1`；IO JSON保持`issue_interval_ns=1`，MPU每ns服务预算仍为190 B（本实验效率1.0）。旧冻结二进制不识别新开关，精确重放应使用其原始命令和输入。','',
        '```bash','python3 simulators/sue/harness/v2/run.py build',
        'python3 simulators/sue/harness/v2/verify_io_wakeup.py results/sue/new-io-wakeup-check',
        '```','',
        '[完整汇总与逐例路径](io-wakeup-data.json)。原始证据位于`/data/scale_up_evaluation-event-wakeup-20261030/`，大型逐AXI轨迹无损压缩留在原目录，不复制进离线手册。']
    text='\n'.join(lines)+'\n';(dest/'io-wakeup.md').write_text(text)
    try:
        from markdown_it import MarkdownIt
        body=MarkdownIt('commonmark').enable('table').render(text)
        (dest/'io-wakeup.html').write_text('<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>IO背压事件唤醒验证</title><style>body{font:16px/1.7 system-ui;max-width:1100px;margin:40px auto;padding:0 20px;color:#19304a}table{border-collapse:collapse;display:block;overflow:auto}th,td{padding:9px 13px;border-bottom:1px solid #dce5ed}th{background:#eef4f8}pre{overflow:auto;background:#f1f5f9;padding:16px}a{color:#076b9a}</style><body>'+body+'</body></html>')
    except ImportError: pass
    print(dest/'io-wakeup.md')

if __name__=='__main__':main()
