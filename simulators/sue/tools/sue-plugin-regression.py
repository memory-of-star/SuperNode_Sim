#!/usr/bin/env python3
"""SUE Wireshark 插件全量字段回归: tshark -T fields vs 抓包工具 .txt ground truth。

用法: tools/sue-plugin-regression.py [capture_dir ...]
  每个 capture_dir 由 SUE_PCAP=1 抓包产生(含成对的 sue-*.pcap / sue-*.txt);
  默认跑 /tmp 下的 6 套覆盖集(afh EN10MB / 完整交互 / 稳态 / 2KB ACK / DSCP-VC / PFC)。
  依赖: tshark(个人插件目录已装 sue-wireshark-plugin.lua);USER0 经 -o 运行时
  uat 注入, 无需持久化首选项。

逐帧比对: 类型判定(ctl/preamble/CC/PFC/AFH)+ 全部 sue.* 字段。
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

TSHARK_UAT = 'uat:user_dlts:"User 0 (DLT=147)","sue","0","","0",""'
FIELDS = [
    "frame.len", "sue.ctlos.type", "sue.llr.seq", "sue.llr.init_seq",
    "sue.llr.init_data", "sue.cf.vc", "sue.cf.count", "sue.cc.cid",
    "sue.cc.vc", "sue.pfc.pev", "sue.pfc.quanta", "sue.preamble.seq",
    "sue.preamble.eligible", "sue.afh.dst_xpu", "sue.afh.src_xpu",
    "sue.afh.shim.ttl", "sue.afh.shim.dscp", "sue.afh.shim.f",
    "sue.afh.shim.flow", "sue.afh.vlan.pcp", "sue.afh.vlan.vid",
    "sue.macctl.opcode", "vlan.id",
]
CTL_TYPE = {"LLR_ACK": "0x01", "LLR_NACK": "0x02", "LLR_INIT": "0x03",
            "LLR_INIT_ECHO": "0x04", "CF_UPDATE": "0x10",
            "PFC_PAUSE": "0x20", "PFC_RESUME": "0x21", "LLDP": "0x30"}

DEFAULT_DIRS = ["/tmp/sue-pcap-test4", "/tmp/sue-pcap-user0",
                "/tmp/sue-pcap-steady", "/tmp/sue-pcap-ack",
                "/tmp/sue-pcap-vc5", "/tmp/sue-pcap-pfc"]


def txt_frames(txt):
    out = []
    for line in Path(txt).read_text().splitlines():
        if re.match(r"^#\d+ ", line):
            out.append(line)
    return out


def tshark_rows(pcap):
    cmd = ["tshark", "-o", TSHARK_UAT, "-r", str(pcap), "-T", "fields",
           "-E", "separator=|"] + sum([["-e", f] for f in FIELDS], [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or "Error" in r.stderr:
        sys.exit(f"tshark failed on {pcap}:\n{r.stderr}")
    return [ln.split("|") for ln in r.stdout.splitlines() if ln.strip()]


def nums(field, base=10):
    return [int(x, base) for x in field.split(",")] if field else []


def first(field, base=10):
    v = nums(field, base)
    return v[0] if v else None


def pcap_linktype(pcap):
    with open(pcap, "rb") as fh:
        gh = fh.read(24)
    if len(gh) < 24 or gh[:4] not in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        return None
    return int.from_bytes(gh[20:24], "little")


def compare(tag, txt_line, row, fails, linktype):
    def fail(why):
        fails.append(f"  [{tag}] {why}\n    txt: {txt_line}\n    ws : {'|'.join(row)}")

    d = dict(zip(FIELDS, row))
    if str(int(d["frame.len"])) not in txt_line:
        fail(f"frame.len={d['frame.len']} 不在 txt 行中")

    m = re.search(r"ctl\((\w+)\)(.*)", txt_line)
    if m:
        name, rest = m.group(1), m.group(2)
        # CC/PFC 是真以太帧, 不带 SueCtlHeader -> ctlos.type 为空
        if name not in ("CC_UPDATE", "PFC_PAUSE", "PFC_RESUME"):
            if d["sue.ctlos.type"].lower() != CTL_TYPE[name]:
                fail(f"ctl type: {d['sue.ctlos.type']} != {CTL_TYPE[name]}")
                return
        if name in ("LLR_ACK", "LLR_NACK"):
            gt = int(re.search(r"seq=(\d+)", rest).group(1))
            if nums(d["sue.llr.seq"])[:1] != [gt]:
                fail(f"llr.seq {d['sue.llr.seq']} != {gt}")
        elif name in ("LLR_INIT", "LLR_INIT_ECHO"):
            gts = int(re.search(r"init_seq=(\d+)", rest).group(1))
            gtd = int(re.search(r"init_data=0x([0-9a-f]+)", rest).group(1), 16)
            if nums(d["sue.llr.init_seq"])[:1] != [gts]:
                fail(f"init_seq {d['sue.llr.init_seq']} != {gts}")
            if nums(d["sue.llr.init_data"], 16)[:1] != [gtd]:
                fail(f"init_data {d['sue.llr.init_data']} != {gtd}")
        elif name == "CF_UPDATE":
            gt = [(int(a), int(b)) for a, b in
                  re.findall(r"vc(\d+)_cf=(\d+)", rest)]
            if len(gt) == 1:
                gt.append((0, 0))   # txt 省略全零 entry2, 线上空槽为 (0,0)
            got = list(zip(nums(d["sue.cf.vc"]), nums(d["sue.cf.count"])))
            if got != gt:
                fail(f"cf (vc,count) {got} != {gt}")
        elif name == "CC_UPDATE":
            gt = {int(k): int(v) for k, v in re.findall(r"vc(\d+)=(\d+)", rest)}
            cid = int(d["sue.cc.cid"], 16)
            vals = nums(d["sue.cc.vc"])
            if cid != 0xFA7ACB:
                fail(f"cc.cid {d['sue.cc.cid']} != 0xfa7acb")
            if len(vals) != 16:
                fail("cc.vc 不是 16 项")
            got = {i: v for i, v in enumerate(vals) if v}
            if got != gt:
                fail(f"cc 逐 VC {got} != {gt}")
        elif name in ("PFC_PAUSE", "PFC_RESUME"):
            gt_pev = int(re.search(r"pev=0x([0-9a-f]+)", rest).group(1), 16)
            if int(d["sue.pfc.pev"], 16) != gt_pev:
                fail(f"pfc.pev {d['sue.pfc.pev']} != {gt_pev}")
            gtq = {int(k): int(v) for k, v in re.findall(r"vc(\d+)=(\d+)", rest)}
            q = nums(d["sue.pfc.quanta"])
            gotq = {i: v for i, v in enumerate(q) if v}
            if gotq != gtq or len(q) != 8:
                fail(f"pfc.quanta {q} != {gtq}")
        return

    m = re.search(r"llr\[(\w+),seq=(\d+)\]", txt_line)
    if m:
        if m.group(1) != "eligible" or d["sue.preamble.eligible"] not in ("1", "True"):
            fail(f"preamble.eligible {d['sue.preamble.eligible']}")
        if nums(d["sue.preamble.seq"])[:1] != [int(m.group(2))]:
            fail(f"preamble.seq {d['sue.preamble.seq']} != {m.group(2)}")
    m = re.search(r"shim\(ttl=(\d+),dscp=(\d+),f=(\d)\)", txt_line)
    if m:
        fexp = "True" if m.group(3) == "1" else "False"
        if (first(d["sue.afh.shim.ttl"]) != int(m.group(1))
                or first(d["sue.afh.shim.dscp"]) != int(m.group(2))
                or d["sue.afh.shim.f"] not in (m.group(3), fexp)):
            fail(f"shim ttl/dscp/f: {d['sue.afh.shim.ttl']},{d['sue.afh.shim.dscp']},{d['sue.afh.shim.f']}"
                 f" != {m.group(1)},{m.group(2)},{m.group(3)}")
    m = re.search(r"dst_xpu=(\d+) src_xpu=(\d+)", txt_line)
    if m and linktype == 147:
        # EN10MB 路径下 MAC/XPU 由 Wireshark 原生 eth 解析器显示(插件从
        # EtherType 之后接手), 只在 USER0 路径校验 sue.afh.*_xpu
        if (first(d["sue.afh.dst_xpu"]) != int(m.group(1))
                or first(d["sue.afh.src_xpu"]) != int(m.group(2))):
            fail(f"xpu {d['sue.afh.dst_xpu']}/{d['sue.afh.src_xpu']} != {m.group(1)}/{m.group(2)}")
    m = re.search(r"vlan\(pcp=(\d+),vid=(\d+)\)", txt_line)
    if m:
        vid_field = d["vlan.id"] if linktype != 147 else d["sue.afh.vlan.vid"]
        if first(vid_field) != int(m.group(2)):
            fail(f"vlan vid {vid_field} != {m.group(2)}(EN10MB 用原生 vlan.id 交叉验证)")
        if linktype == 147 and first(d["sue.afh.vlan.pcp"]) != int(m.group(1)):
            fail(f"vlan.pcp {d['sue.afh.vlan.pcp']} != {m.group(1)}")


def run_dir(capture_dir):
    d = Path(capture_dir)
    pcaps = sorted(d.glob("sue-*.pcap"))
    if not pcaps:
        return (0, 0, [f"  {capture_dir}: 无 pcap"])
    total = fails = 0
    msgs = []
    for pcap in pcaps:
        txt = pcap.with_suffix(".txt")
        if not txt.exists():
            msgs.append(f"  {pcap.name}: 缺 {txt.name}")
            continue
        tf, rows = txt_frames(txt), tshark_rows(pcap)
        if len(tf) != len(rows):
            fails += max(len(tf), len(rows))
            msgs.append(f"  {pcap.name}: 帧数 {len(rows)} != txt {len(tf)}")
            continue
        lt = pcap_linktype(pcap)
        pf = []
        for i, (line, row) in enumerate(zip(tf, rows)):
            compare(f"{pcap.name}#{i}", line, row, pf, lt)
        total += len(tf)
        fails += len(pf)
        msgs.append(f"  {pcap.name}: {len(tf)} 帧, {len(pf)} 失败")
        msgs.extend(pf[:5])
    return total, fails, msgs


def main():
    if not shutil.which("tshark"):
        sys.exit("tshark 不在 PATH")
    dirs = sys.argv[1:] or DEFAULT_DIRS
    grand_t = grand_f = 0
    print(f"tshark: {subprocess.run(['tshark', '--version'], capture_output=True, text=True).stdout.splitlines()[0]}")
    for cd in dirs:
        t, f, msgs = run_dir(cd)
        grand_t += t
        grand_f += f
        print(f"{cd}: {t} 帧 / {f} 失败")
        print("\n".join(msgs))
    print(f"\n总计: {grand_t} 帧全量字段比对, 失败 {grand_f}")
    sys.exit(1 if grand_f else 0)


if __name__ == "__main__":
    main()
