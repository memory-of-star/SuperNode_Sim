# tools/ — SUE 仿真辅助工具

## sue-wireshark-plugin.lua — Wireshark 报文解析插件(全格式)

解析 ns-3 SUE 仿真抓包(`scratch/sue-scratch-pcap.h`,`SUE_PCAP=1`)的报文格式。

### 支持的格式

| 格式 | 线布局 | 接入方式 |
|---|---|---|
| LLR 数据帧 | 8B 前导(B0=0xFB…B7=0xD5)+ AFH | DLT_USER0 |
| AFH Gen1 | [DA][SA][VLAN?][EtherType 0xFCF1/0xFCF2][shim?][payload] | DLT_USER0 / EtherType |
| AFH shim | [AR\|F\|C\|CNM\|TTL(4b)] [DSCP(6b)\|ECN(2b)] [FlowLabel iff F=1] | 同上 |
| LLR CtlOS | [类型 1B][载荷 8B]:ACK/NACK(seq)、INIT/INIT_ECHO(init_seq+init_data) | DLT_USER0 |
| CF_Update | CtlOS 0x10,两条 (VC 5b, CF_count 15b) | DLT_USER0 |
| CC_Update | 真以太 64B(EtherType 0x8808 + Opcode 0xFFFE),CID + 16×20bit 逐 VC CC + FCS | DLT_USER0 |
| PFC | 真以太 64B(Opcode 0x0101),PEV + 8×2B quanta + FCS | DLT_USER0 |

### 安装

复制到 Wireshark 个人插件目录后重启 Wireshark:

- Linux:`~/.local/lib/wireshark/plugins/sue-wireshark-plugin.lua`
- Windows:`%APPDATA%\Wireshark\plugins\sue-wireshark-plugin.lua`
- macOS:`~/.local/lib/wireshark/plugins/sue-wireshark-plugin.lua`

### 使用

```bash
# 完整交互(前导/CtlOS/AFH/CC/PFC 全部解析)——必须用 USER0 DLT:
SUE_PCAP=1 SUE_PCAP_DLT=user0 ./ns3.36.1-sue-cbfc-cc-update-onwire-default
# 纯 AFH 链路(默认 EN10MB):Wireshark 原生解析 eth/VLAN,插件解析 shim:
SUE_PCAP=1 ./ns3.36.1-sue-afh-forward-default
```

USER0 pcap 首次打开需一次性配置 DLT_USER 封装(否则报
"User encapsulation not handled: DLT=147"):**编辑 → 首选项 → Protocols →
DLT_USER → User encapsulations → Edit… → +**,DLT 选 `User 0 (DLT=147)`,
Payload protocol 填 `sue`,确定后重开 pcap。插件加载时也会尝试自动注册,
若自动注册生效则无需此步。

**DLT 选型规则**(评审结论):**含前导码/CtlOS 的场景一律 `SUE_PCAP_DLT=user0`
+ 主插件整帧自定义解析**(单一视图、无原生 malformed 噪声;CC_Update/PFC 等
无前导真以太帧在该路径内按以太结构解析,Opcode 显示 CC_Update/PFC,不会出现
原生 "Unknown")。`en10mb` 仅用于两类场景:纯 AFH 链路(如 afh-forward,原生
eth/VLAN + 插件 EtherType 接入解 shim)和只关心 CC_Update/PFC 的以太流
(PFC 原生直读,CC 字段装独立脚本)。曾尝试的 EN10MB 兜底 postdisector
(原生树 + 补解树混合展示)因可读性差已撤销。

过滤示例:`sue.llr.seq == 1`、`sue.ctlos.type == 0x10`、`sue.cc.vc > 0`、`sue.afh.shim.ttl < 3`。

### 偏好项(编辑 → 首选项 → Protocols → SUE)

- `SUE EtherType 0/1`:两个 AFH EtherType 值(默认 0xFCF1/0xFCF2;改动需重载插件);
- `EtherType 0/1 carries AFH shim`:逐 EtherType 的 shim 有无(默认 无/有,与仿真
  `SueConfig` 默认一致),实时生效。

### 验证状态

- **tshark 4.6.4 全量回归通过**:`sue-plugin-regression.py` 对 6 套实抓 pcap(afh
  EN10MB / cc-update-onwire 完整交互 / 稳态 / 2KB ACK 节拍 / DSCP-VC / PFC 门控)
  共 **9718 帧**抽取全部 `sue.*` 字段与抓包工具 `.txt`(C++ `Deserialize` 同源)
  逐帧比对,**失败 0**;含 PFC 分支真字节验证与 5 类显示过滤器计数核对。
- 回归用法:抓包目录(`sue-*.pcap`+`.txt` 成对)作为参数,USER0 经 `-o uat:` 运行时
  注入,**无需任何持久化首选项**;持久化文件格式为 6 字段
  `"User 0 (DLT=147)","sue","0","","0",""`(`~/.config/wireshark/user_dlts`)。
- 其余 Wireshark 版本未逐一验证(插件语法按 Lua 5.2 兼容收敛);若加载或解析报错,
  把 Wireshark/tshark 的 Lua 报错文本反馈即可定位。

## cc-update-wireshark.lua — CC_Update 独立解析脚本(免全套插件)

只想在**普通 EN10MB pcap** 里看 UEC CC_Update(EtherType 0x8808 + Opcode 0xFFFE)
字段时的轻量选择:单装这一个文件即可,零配置、无需 DLT_USER。实现为
postdissector(原生 eth/macctrl 解完后追加树;Wireshark 的 MAC Control 无
opcode 子表,0xFFFE 原生显示 Unknown)。字段前缀 `suecc.*`,与主插件
`sue.*` 互不冲突、可共存;USER0 pcap 已由主插件解析为 SUE-CC 时自动让位。

```bash
tshark -r sue-steady-a-b.pcap -Y "suecc.vc" -T fields -e suecc.cid -e suecc.vc
```

注:①**走哪条解析路线由 pcap 的 linktype 决定**——`SUE_PCAP_DLT=user0` 抓的包
整帧先给主 SUE 插件(显示 SUE-CC),本脚本按设计让位;要纯以太路线(原生 eth +
suecc.*)就用默认 en10mb 抓包(省略 SUE_PCAP_DLT)。②协议/Info 列保留原生
"MAC CTRL / Unknown"(tshark 下 postdissector 覆盖列不生效),字段树、
`suecc.*` 过滤与提取不受影响。

## 与原生解析器的分工(实测 Wireshark 4.6.4)

| 帧型 | 原生 Wireshark(EN10MB,不装任何脚本) | 需要 SUE 侧 |
|---|---|---|
| eth / VLAN / FCS | ✅ 原生 | — |
| **PFC**(0x8808/0x0101) | ✅ 原生完整解析(Class Based Flow Control:使能向量+逐类暂停时间) | 不需要 |
| CC_Update 外壳(0x8808/0xFFFE 的 eth 层) | ✅ 原生 | — |
| CC_Update 字段(CID/16×20bit 逐 VC) | ❌ Unknown opcode | `cc-update-wireshark.lua` 或主插件 |
| AFH(0xFCF1/2 + shim) | ❌ Unknown ethertype | 主插件(EtherType 接入) |
| LLR 前导 / CtlOS(非以太帧) | ❌ 判 malformed | 主插件 + **USER0 DLT**(选型规则见上) |

注:控制帧 MACSA 已修复为 IEEE 合规单播(`GetMacSa` 对非单播默认地址合成
`02:00:00:<ifIndex>:<nodeId>`,本地管理单播;原生解析器对组地址 SA 的
Expert Warning 消除)。
