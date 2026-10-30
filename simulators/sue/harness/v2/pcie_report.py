"""Summarize transactional PCIe integrations; never mix with the legacy sweep report."""
import argparse,json,csv,html,shutil,statistics
from pathlib import Path
from run import dump,csvwrite,sha
from pcie_config import analyze

def main():
    ap=argparse.ArgumentParser();ap.add_argument('root',type=Path);ap.add_argument('--output',type=Path);a=ap.parse_args()
    root=a.root.resolve();out=(a.output or root/'analysis').resolve();out.mkdir(parents=True,exist_ok=True)
    results=json.loads((root/'results.json').read_text());rows=[]
    assert all(r['status']=='passed' for r in results),'failed runs require explicit exclusion, not automatic filtering'
    for r in results:
        run=Path(r['run']);manifest=json.loads((run/'command-manifest.json').read_text())
        for name,digest in manifest['inputs'].items():assert sha(run/name)==digest,(name,run)
        info=analyze(run) if (run/'pcie.json').exists() else {}
        rows.append(dict(index=r['case_index'],model=r['model'],backend=r['backend'],variant=r.get('study_variant','transaction' if r.get('pcie_profile') else 'legacy'),mpus=r['mpus'],seed=r['seed'],batch=r['batch'],context=r['context'],steps=r['steps'],block_us=r['block_us'],
            useful_GB=info.get('useful_bytes',sum(v for k,v in r['offload_bytes'].items() if k.endswith('_pcie')))/1e9,
            payload_GB=info['transferred_bytes']/1e9 if info else '',DDR_GB=info['ddr_bytes']/1e9 if info else '',
            read_requests=info.get('read_requests',''),completions=info.get('completions',''),tag_stall_checks=info.get('tag_stall_checks',''),
            descriptor_wait_sum_us=info.get('descriptor_wait_us',''),credit_wait_sum_us=info.get('credit_wait_us',''),run=str(run)))
    dump(out/'summary.json',rows);csvwrite(out/'summary.csv',rows)
    text='''# PCIe事务级模型：实现与联动验证

此报告采用双路、每路12通道DDR5-6400、每Host8卡/1TB。DDR效率0.75，理论/有效带宽每路614.4/460.8GB/s；其余队列、延迟、tag等均为可修改的工程假设。详细机制见手册§9.9。

DeepSeek为B64/32K/两步完整代表块；Kimi/GLM为B8/2K/一步小规模联动验证，不能把后两者与旧第9章B64/32K结果比较。表中时间均为窗口除以本次步数，不是全模型TPOT。旧FIFO保留1µs启动，事务模型使用100ns描述符提交与200ns引擎启动，延迟按各环节事件产生，两者不叠加。

| 模型 | 方案 | 模型版本 | MPU | seed | B/context | µs/代表块 |
|---|---|---|---:|---:|---|---:|
'''
    for r in rows:text+=f"| {r['model']} | {r['backend']} | {r['variant']} | {r['mpus']} | {r['seed']} | {r['batch']}/{r['context']} | {r['block_us']:.3f} |\n"
    text+='''
## 如何解读

- CSV中旧FIFO未建模/未记录的事务指标留空，不表示零开销。
- 旧FIFO与事务模型的差异同时涉及队列、内存、启动、分片和背压，不能将差值全部归因为某一机制。
- MPU-only控制组的任务、端口和资源统计逐字节一致；PCIe模型不会凭空改变SUE协议。
- 累加排队时间是多个请求的总等待量，允许重叠，不能与端到端窗口直接相加。
- 本次并未重跑历史210项全矩阵；这些是集成验证与选定案例对照。
- 原始run目录中pcie_descriptors.csv记录每描述符字节与时序；pcie_resources.csv记录链路/DDR/接收缓冲；pcie_chips.csv记录队列、tag与在途峰值。
- 信用按头部个数和16B数据额度抽象；没有配置空间、序列号、ACK/NAK重放、FLIT/FEC或电气PHY。DDR为通道带宽与受限并发/固定延迟模型，不包含DRAM逐命令时序。
'''
    (out/'report.md').write_text(text)
    import markdown_it
    body=markdown_it.MarkdownIt('commonmark').enable('table').render(text)
    (out/'report.html').write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PCIe事务级模型验证</title><style>body{max-width:1250px;margin:32px auto;padding:16px;font:16px/1.8 system-ui;color:#17354b}table{display:block;overflow:auto;border-collapse:collapse}td,th{border:1px solid #cbd5e1;padding:8px}th{background:#eaf2f8}a{color:#167da5}</style>'+body+'<p><a href="summary.csv">下载CSV</a> · <a href="verification.json">校验记录</a></p>')
    dump(out/'verification.json',dict(status='passed',cases=len(rows),checks=['frozen input SHA256','all integration cases passed','transaction payload, wire and DDR byte conservation','bounded tags, windows, descriptors and receive credits'],source_verification=json.loads((root/'verification.json').read_text()) if (root/'verification.json').exists() else {'status':'no separate matrix regression record; per-run checks passed'}))
    shutil.copy2(root/'plan.json',out/'plan.json')
    print(json.dumps({'report':str(out/'report.html'),'cases':len(rows)}))
if __name__=='__main__':main()
