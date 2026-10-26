#!/usr/bin/env python3
"""Adaptive addendum: separate credit from auto-sized switch egress buffer."""
import json,sys,shutil
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from explore import workload,augment
from run import create,run_case,dump
TEXT='''# 追加实验：固定交换机出口缓冲的信用扫描

这是初轮执行期间核对源码后设计的追加对照，不属于最初348项的预注册矩阵。初轮所有案例保持原样。原配置CBFC_EGRESS_BUF_BYTES=0表示auto，交换机每VC出口缓冲=creditLimit×256B，因此16/64/512信用同时对应4/16/128KiB。初轮congestion组只能解释联合变化。

追加18项：128 parity/rotate × seed1/2/3 × 信用16/64/512；相同32源、8MiB incast，源队列64KiB、2048B分片、UCIe400G/64KiB/20ns。将sue.conf中CBFC_EGRESS_BUF_BYTES显式设为131072，固定每VC出口128KiB，仅改变信用。完整配置在命令快照中。

预测：若初轮信用变化的效果主要由缓冲变化造成，固定缓冲后曲线会改变；若仍有差异，支持信用门控本身影响当前incast。与原512信用的auto模式物理缓冲等价，可作为复现控制。检查次数仍非时间或丢包。保留失败，不改截止或根据结果挑种子。
'''

def main(root):
    root=Path(root).resolve();results=json.loads((root/'results.json').read_text());assert len(results)==348
    initial=json.loads((root/'design.json').read_text());assert len(initial)==348
    shutil.copy2(root/'design.json',root/'design-initial.json');shutil.copy2(root/'plan.md',root/'plan-initial.md')
    (root/'credit-addendum-plan.md').write_text(TEXT)
    defs=[]
    for credit in (16,64,512):
        for seed in (1,2,3):
            for topo in ('xpu128-parity','xpu128-rotate'):
                defs.append(dict(study='credit_fixed_buffer',variant=str(credit),model='incast',tokens=256,compute=0,profile='uniform',placement='spread',settings={'credits':credit},seed=seed,topology=topo,probe=True,schedule='burst',switch_egress_bytes=131072))
    dump(root/'design.json',initial+defs);dump(root/'credit-addendum-design.json',defs)
    (root/'plan.md').write_text((root/'plan.md').read_text()+'\n## 执行中追加对照\n\n发现自动出口缓冲与信用耦合后，单独登记18项固定缓冲对照，见[credit-addendum-plan.md](credit-addendum-plan.md)。初轮计划原文为plan-initial.md。合计366项，不将追加对照声称为初始预注册。\n')
    def execute(d):
        tasks,routes,meta=workload(d);meta['exploration']=d
        case=root/'cases'/f'credit-fixed-{d["variant"]}-s{d["seed"]}-{d["topology"]}'
        create(case,d['topology'],tasks,meta,dict(seed=d['seed'],stop_ns=5000000,timeout_s=60,**d['settings']),routes)
        p=case/'sue.conf';s=p.read_text();assert 'CBFC_EGRESS_BUF_BYTES 0\n' in s;p.write_text(s.replace('CBFC_EGRESS_BUF_BYTES 0\n','CBFC_EGRESS_BUF_BYTES 131072\n'))
        (case/'experiment-spec.md').write_text(TEXT+'\n'+json.dumps(d,indent=2)+'\n')
        return augment(run_case(case),d)
    with ThreadPoolExecutor(max_workers=3) as pool:
        for future in as_completed([pool.submit(execute,d) for d in defs]):
            result=future.result();results.append(result);dump(root/'results.json',results)
            print(json.dumps(dict(done=len(results),total=366,study=result['study'],variant=result['variant'],topology=result['topology'],status=result['status']),ensure_ascii=False),flush=True)

if __name__=='__main__':main(sys.argv[1])
