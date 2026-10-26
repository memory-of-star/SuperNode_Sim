-- sue-wireshark-plugin.lua — SUE Scale-Up Ethernet 报文解析插件(Wireshark Lua)
--
-- 解析 ns-3 SUE 仿真(scratch/sue-scratch-pcap.h,SUE_PCAP=1)抓的报文:
--   - 数据帧: 8B LLR 前导(B0=0xFB..B7=0xD5) + AFH([DA][SA][VLAN?][EtherType][shim?][payload])
--   - CtlOS(9B, 非以太帧): [类型 1B][载荷 8B]
--       0x01 LLR_ACK / 0x02 LLR_NACK / 0x03 LLR_INIT / 0x04 LLR_INIT_ECHO (LLR 载荷)
--       0x10 CF_UPDATE (CBFC 载荷: 两条 VC+CF_count)
--   - 真以太控制帧(64B, EtherType=0x8808):
--       Opcode=0xFFFE CC_Update (CID + 16x20bit 逐 VC CC 计数 + FCS)
--       Opcode=0x0101 PFC      (PEV + 8x2B quanta + FCS)
--
-- 接入方式(两种, 同时生效):
--   1) DLT_USER0(linktype 147): 抓包时 SUE_PCAP_DLT=user0。需一次性配置
--      Wireshark: 编辑->首选项->Protocols->DLT_USER->User encapsulations
--      ->Edit->+, DLT 选 "User 0 (DLT=147)", Payload protocol 填 "sue",
--      OK 后重新打开 pcap(插件加载时也会尝试自动注册 dlt_user 表)。
--      前导/CtlOS 这类非以太帧只有本路径可解析;
--   2) EtherType 0xFCF1/0xFCF2: 普通 EN10MB pcap(如 afh-forward 抓包)中,
--      以太层后的 AFH shim 部分由本插件解析, 无需任何配置。
--
-- 安装: 复制到 Wireshark 个人插件目录后重启(无需编译):
--   Linux:   ~/.local/lib/wireshark/plugins/sue-wireshark-plugin.lua
--   Windows: %APPDATA%\Wireshark\plugins\sue-wireshark-plugin.lua
--   macOS:   ~/.local/lib/wireshark/plugins/sue-wireshark-plugin.lua
--
-- 位级布局与仿真线代码逐字节对齐(sue-llr-preamble-header / sue-header /
-- sue-shim-header / sue-ctl-header / sue-llr-ctlos-header / sue-cbfc-header /
-- sue-cbfc-cc-header / sue-pfc-header 的 Serialize 实现)。
-- 仅用 Lua 5.2 兼容语法(无位运算符), 适配 Wireshark 内置各版本 Lua。

local p_sue = Proto("sue", "SUE Scale-Up Ethernet")
-- 全局声明: 独立脚本 cc-update-wireshark.lua 检测 SUE_PLUGIN_LOADED 让位
-- (主插件在场时其 USER0 路径已覆盖 CC_Update)。
SUE_PLUGIN_LOADED = true

-- 偏好: shim 是否随 EtherType 出现(仿真里是逐 etype 配置, 默认 0xFCF1 无 / 0xFCF2 有)
p_sue.prefs.eth_type0 = Pref.uint("SUE EtherType 0 (default 0xFCF1)", 0xFCF1)
p_sue.prefs.eth_type1 = Pref.uint("SUE EtherType 1 (default 0xFCF2)", 0xFCF2)
p_sue.prefs.shim0     = Pref.bool("EtherType 0 carries AFH shim", false)
p_sue.prefs.shim1     = Pref.bool("EtherType 1 carries AFH shim", true)

local f = {
  -- LLR 前导
  pre        = ProtoField.none ("sue.preamble",       "SUE LLR Preamble"),
  pre_elig   = ProtoField.bool ("sue.preamble.eligible", "LLR Eligible"),
  pre_seq    = ProtoField.uint32("sue.preamble.seq",  "LLR Sequence Number", base.DEC),
  pre_pad    = ProtoField.bytes("sue.preamble.pad",   "Preamble Pad"),
  -- AFH
  afh_dst    = ProtoField.ether("sue.afh.dst",        "Destination"),
  afh_src    = ProtoField.ether("sue.afh.src",        "Source"),
  afh_dstxpu = ProtoField.uint32("sue.afh.dst_xpu",   "Destination XPU-id (DA 高 10 位)", base.DEC),
  afh_srcxpu = ProtoField.uint32("sue.afh.src_xpu",   "Source XPU-id (SA 高 10 位)", base.DEC),
  vlan       = ProtoField.none ("sue.afh.vlan",       "802.1Q VLAN"),
  vlan_pcp   = ProtoField.uint8("sue.afh.vlan.pcp",   "PCP", base.DEC),
  vlan_dei   = ProtoField.bool ("sue.afh.vlan.dei",   "DEI"),
  vlan_vid   = ProtoField.uint16("sue.afh.vlan.vid",  "VID", base.DEC),
  afh_etype  = ProtoField.uint16("sue.afh.etype",     "EtherType", base.HEX),
  -- shim (b0=[AR|F|C|CNM|TTL(4)] b1=[DSCP(6)|ECN(2)] [b2=FlowLabel iff F])
  shim       = ProtoField.none ("sue.afh.shim",       "SUE AFH Shim"),
  shim_ar    = ProtoField.bool ("sue.afh.shim.ar",    "AR (adaptive routing eligible)"),
  shim_f     = ProtoField.bool ("sue.afh.shim.f",     "F (3B / Flow Label present)"),
  shim_c     = ProtoField.bool ("sue.afh.shim.c",     "C (CNM generation eligible)"),
  shim_cnm   = ProtoField.bool ("sue.afh.shim.cnm",   "CNM (this frame is a CNM)"),
  shim_ttl   = ProtoField.uint8("sue.afh.shim.ttl",   "TTL", base.DEC),
  shim_dscp  = ProtoField.uint8("sue.afh.shim.dscp",  "DSCP", base.DEC),
  shim_ecn   = ProtoField.uint8("sue.afh.shim.ecn",   "ECN", base.DEC),
  shim_fl    = ProtoField.uint8("sue.afh.shim.flow",  "Flow Label", base.HEX),
  payload    = ProtoField.bytes("sue.afh.payload",    "Payload"),
  -- CtlOS
  ctl_type   = ProtoField.uint8("sue.ctlos.type",     "Type", base.HEX,
    { [1]="LLR_ACK", [2]="LLR_NACK", [3]="LLR_INIT", [4]="LLR_INIT_ECHO",
      [0x10]="CF_UPDATE", [0x20]="PFC_PAUSE", [0x21]="PFC_RESUME", [0x30]="LLDP" }),
  llr_seq    = ProtoField.uint32("sue.llr.seq",       "Sequence Number", base.DEC),
  llr_initseq= ProtoField.uint32("sue.llr.init_seq",  "Init Sequence", base.DEC),
  llr_initdat= ProtoField.uint16("sue.llr.init_data", "Init Data", base.HEX),
  cf_vc      = ProtoField.uint8("sue.cf.vc",          "VC Index", base.DEC),
  cf_count   = ProtoField.uint32("sue.cf.count",      "CF Count", base.DEC),
  -- MAC Control (EtherType 0x8808)
  mc_opcode  = ProtoField.uint16("sue.macctl.opcode", "Opcode", base.HEX,
    { [0xFFFE]="CC_Update", [0x0101]="PFC" }),
  cc_cid     = ProtoField.uint32("sue.cc.cid",        "CID", base.HEX),
  cc_msgtype = ProtoField.uint8("sue.cc.msgtype",     "MsgType", base.HEX,
    { [1]="VC[0:15]" }),
  cc_vc      = ProtoField.uint32("sue.cc.vc",         "CC (consumed-credit counter)", base.DEC),
  pfc_pev    = ProtoField.uint16("sue.pfc.pev",       "Priority Enable Vector", base.HEX),
  pfc_quanta = ProtoField.uint16("sue.pfc.quanta",    "Pause Quanta", base.DEC),
  fcs        = ProtoField.uint32("sue.fcs",           "FCS (CRC-32)", base.HEX),
}
p_sue.fields = f

-- 纯算术位工具(避免位运算符, 兼容 Lua 5.2)。math.floor 收尾保证整数子类型
-- (否则 5.4 下 2^n 参与运算产生浮点, 拼接显示成 "0.0")
local function shr(v, n) return math.floor(v / (2 ^ n)) end
local function bits(v, hi, lo)            -- 取 v 的 [hi:lo] 位
  return math.floor(math.floor(v / (2 ^ lo)) % (2 ^ (hi - lo + 1)))
end

local CTL_NAMES = { [1]="LLR_ACK", [2]="LLR_NACK", [3]="LLR_INIT", [4]="LLR_INIT_ECHO",
                    [0x10]="CF_UPDATE", [0x20]="PFC_PAUSE", [0x21]="PFC_RESUME", [0x30]="LLDP" }

-- CC 16x20bit 连续 MSB-first 打包(40B 起点 base): 取第 idx 个(idx 0..15)
local function cc_value(tvb, base, idx)
  local bitpos = idx * 20
  local byte = base + math.floor(bitpos / 8)
  local shift = bitpos % 8
  local v
  if shift + 20 <= 24 then
    v = shr(tvb(byte, 3):uint(), 4 - shift)
  else
    v = shr(tvb(byte, 4):uint(), 12 - shift)
  end
  return math.floor(v % 0x100000)
end

-- shim + payload(tvb 已定位到 EtherType 之后)
local function dissect_afh_payload(tvb, pinfo, tree)
  local st = tree:add(p_sue, tvb(), "SUE AFH Shim + Payload")
  local b0 = tvb(0, 1):uint()
  local b1 = tvb(1, 1):uint()
  local hasF = bits(b0, 7, 6) == 1
  local sh = st:add(f.shim, tvb(0, hasF and 3 or 2))
  sh:add(f.shim_ar,  tvb(0, 1), bits(b0, 7, 7))
  sh:add(f.shim_f,   tvb(0, 1), bits(b0, 6, 6))
  sh:add(f.shim_c,   tvb(0, 1), bits(b0, 5, 5))
  sh:add(f.shim_cnm, tvb(0, 1), bits(b0, 4, 4))
  sh:add(f.shim_ttl, tvb(0, 1), bits(b0, 3, 0))
  sh:add(f.shim_dscp, tvb(1, 1), bits(b1, 7, 2))
  sh:add(f.shim_ecn,  tvb(1, 1), bits(b1, 1, 0))
  local o
  if hasF then
    sh:add(f.shim_fl, tvb(2, 1))
    o = 3
  else
    o = 2
  end
  if tvb:len() > o then
    st:add(f.payload, tvb(o))
  end
end

-- EtherType 表入口各建一个薄 Proto(表内收不到来源 etype, shim 有无由对应偏好决定;
-- 注意 Wireshark 要求各 Proto 描述文案互不相同)
local function make_afh_entry(name, desc, getShim)
  local p = Proto(name, desc)
  p.dissector = function (buf, pinfo, tree)
    pinfo.cols.protocol = "SUE-AFH"
    if getShim() and buf:len() >= 2 then
      dissect_afh_payload(buf, pinfo, tree)
    else
      local st = tree:add(p, buf())
      st:add(f.payload, buf())
    end
  end
  return p
end

-- CC_Update 字段体(返回非零 VC 摘要文本);以太分支与 EN10MB 兜底共用。
local function dissect_cc_fields (tvb, etOff, st)
  st:add(f.cc_cid, tvb(etOff + 2, 3), tvb(etOff + 2, 3):uint())
  st:add(f.cc_msgtype, tvb(etOff + 5, 1))
  local ccbase = etOff + 6
  local parts = {}
  for i = 0, 15 do
    local v = cc_value(tvb, ccbase, i)
    local it = st:add(f.cc_vc, tvb(ccbase, 40), v)
    it:append_text(" (VC" .. i .. ")")
    if v > 0 then parts[#parts + 1] = "vc" .. i .. "=" .. v end
  end
  st:add(f.fcs, tvb(60, 4))
  return next(parts) and table.concat(parts, ",") or "all-zero"
end

-- 以太形状帧(有 MAC 头): AFH / MAC Control(CC_Update, PFC)
local function dissect_eth_frame(tvb, pinfo, tree)
  local st = tree:add(p_sue, tvb(), "SUE Scale-Up Ethernet")
  local et0 = tvb(12, 2):uint()
  local etOff = 14
  if et0 == 0x8100 then
    local t0 = tvb(13, 1):uint()
    local vt = st:add(f.vlan, tvb(12, 4))
    vt:add(f.vlan_pcp, tvb(13, 1), bits(t0, 7, 5))
    vt:add(f.vlan_dei, tvb(13, 1), bits(t0, 4, 4))
    vt:add(f.vlan_vid, tvb(13, 2), bits(t0, 3, 0) * 256 + tvb(14, 1):uint())
    st:add(f.afh_etype, tvb(16, 2))
    etOff = 18
    et0 = tvb(16, 2):uint()
  end
  local d0 = tvb(0, 1):uint(); local d1 = tvb(1, 1):uint()
  local s0 = tvb(6, 1):uint(); local s1 = tvb(7, 1):uint()
  local dxpu = d0 * 4 + shr(d1, 6)
  local sxpu = s0 * 4 + shr(s1, 6)
  if etOff == 14 then st:add(f.afh_etype, tvb(12, 2)) end

  if et0 == 0x8808 then
    -- MAC Control(CC/PFC): 只显示 MAC, 不做 XPU-id 解码(组播地址无 XPU 语义)
    st:add(f.afh_dst, tvb(0, 6))
    st:add(f.afh_src, tvb(6, 6))
    -- MAC Control
    local op = tvb(etOff, 2):uint()
    local ot = st:add(f.mc_opcode, tvb(etOff, 2), op)
    if op == 0xFFFE then
      pinfo.cols.protocol = "SUE-CC"
      pinfo.cols.info = "CC_Update " .. dissect_cc_fields (tvb, etOff, st)
    elseif op == 0x0101 then
      pinfo.cols.protocol = "SUE-PFC"
      st:add(f.pfc_pev, tvb(etOff + 2, 2))
      for i = 0, 7 do
        local it = st:add(f.pfc_quanta, tvb(etOff + 4 + 2 * i, 2))
        it:append_text(" (VC" .. i .. ")")
      end
      st:add(f.fcs, tvb(60, 4))
      pinfo.cols.info = "PFC pev=0x" .. string.format("%04x", tvb(etOff + 2, 2):uint())
    else
      pinfo.cols.protocol = "SUE-MACCTL"
      pinfo.cols.info = "MAC Control opcode=0x" .. string.format("%04x", op)
    end
  elseif et0 == p_sue.prefs.eth_type0 or et0 == p_sue.prefs.eth_type1 then
    st:add(f.afh_dst, tvb(0, 6))
    st:add(f.afh_src, tvb(6, 6))
    st:add(f.afh_dstxpu, tvb(0, 2), dxpu)
    st:add(f.afh_srcxpu, tvb(6, 2), sxpu)
    pinfo.cols.protocol = "SUE-AFH"
    local hasShim = (et0 == p_sue.prefs.eth_type1 and p_sue.prefs.shim1)
                 or (et0 == p_sue.prefs.eth_type0 and p_sue.prefs.shim0)
    if hasShim and tvb:len() > etOff then
      dissect_afh_payload(tvb(etOff):tvb(), pinfo, st)
    elseif tvb:len() > etOff then
      st:add(f.payload, tvb(etOff))
    end
    pinfo.cols.info = "AFH dst_xpu=" .. dxpu .. " src_xpu=" .. sxpu
  else
    pinfo.cols.protocol = "SUE"
    if tvb:len() > etOff then st:add(f.payload, tvb(etOff)) end
  end
end

-- CtlOS: [type 1B][payload 8B]
local function dissect_ctlos(tvb, pinfo, tree)
  local st = tree:add(p_sue, tvb(), "SUE CtlOS")
  local t = tvb(0, 1):uint()
  st:add(f.ctl_type, tvb(0, 1))
  if t == 1 or t == 2 then         -- ACK / NACK
    local seq = bits(tvb(2, 1):uint(), 7, 4) * 65536 + tvb(3, 1):uint() * 256 + tvb(4, 1):uint()
    st:add(f.llr_seq, tvb(2, 3), seq)
    pinfo.cols.info = CTL_NAMES[t] .. " seq=" .. seq
  elseif t == 3 or t == 4 then     -- INIT / INIT_ECHO
    local seq = bits(tvb(2, 1):uint(), 7, 4) * 65536 + tvb(3, 1):uint() * 256 + tvb(4, 1):uint()
    st:add(f.llr_initseq, tvb(2, 3), seq)
    st:add(f.llr_initdat, tvb(5, 2))
    pinfo.cols.info = CTL_NAMES[t] .. " init_seq=" .. seq
  elseif t == 0x10 then            -- CF_UPDATE: D0-D1 保留, 两条 (VC,CF)
    local d2, d3, d4 = tvb(3, 1):uint(), tvb(4, 1):uint(), tvb(5, 1):uint()
    local d5, d6, d7 = tvb(6, 1):uint(), tvb(7, 1):uint(), tvb(8, 1):uint()
    local vc1 = bits(d2, 7, 3)
    local cf1 = bits(d2, 2, 0) * 4096 + d3 * 16 + shr(d4, 4)
    local vc2 = bits(d5, 7, 3)
    local cf2 = bits(d5, 2, 0) * 4096 + d6 * 16 + shr(d7, 4)
    local i1 = st:add(f.cf_vc, tvb(3, 3), vc1); i1:append_text(" entry1")
    st:add(f.cf_count, tvb(3, 3), cf1):append_text(" entry1")
    local i2 = st:add(f.cf_vc, tvb(6, 3), vc2); i2:append_text(" entry2")
    st:add(f.cf_count, tvb(6, 3), cf2):append_text(" entry2")
    pinfo.cols.info = "CF_UPDATE vc" .. vc1 .. "_cf=" .. cf1 .. " vc" .. vc2 .. "_cf=" .. cf2
  else
    pinfo.cols.info = (CTL_NAMES[t] or ("CTL(0x" .. string.format("%02x", t) .. ")"))
  end
  pinfo.cols.protocol = "SUE-CTL"
end

p_sue.dissector = function (buffer, pinfo, tree)
  local len = buffer:len()
  local b0 = len > 0 and buffer(0, 1):uint() or 0
  if len == 9 and CTL_NAMES[b0] then
    dissect_ctlos(buffer, pinfo, tree)
  elseif len >= 8 and b0 == 0xFB and buffer(7, 1):uint() == 0xD5
      and buffer(4, 3):uint() == 0x555555 then
    -- LLR 数据帧: 8B 前导 + AFH
    local pre = tree:add(f.pre, buffer(0, 8))
    local b1 = buffer(1, 1):uint()
    local elig = (b1 % 16) == 7
    pre:add(f.pre_elig, buffer(1, 1), elig)
    if elig then
      pre:add(f.pre_seq, buffer(1, 3), shr(b1, 4) * 65536 + buffer(2, 1):uint() * 256 + buffer(3, 1):uint())
    end
    pre:add(f.pre_pad, buffer(4, 3))
    local rest = buffer(8):tvb()
    dissect_eth_frame(rest, pinfo, tree)
    local seq = elig and (shr(b1, 4) * 65536 + buffer(2, 1):uint() * 256 + buffer(3, 1):uint()) or -1
    pinfo.cols.protocol = "SUE-DATA"
    pinfo.cols.info = "Data llr_seq=" .. seq
  elseif len >= 14 then
    dissect_eth_frame(buffer, pinfo, tree)
  else
    tree:add(p_sue, buffer(), "SUE: 无法识别的短帧 (" .. len .. "B)")
    pinfo.cols.protocol = "SUE"
  end
end

-- 接入 1: DLT_USER0(147) 整帧(SUE 链路线格式, 含前导/CtlOS)
-- DLT_USERn 有专用机制: frame 层优先查 "dlt_user" 表(由首选项
-- Protocols->DLT_USER 的 payload protocol 配置驱动)。两步都做:
-- a) 机会性注册 "dlt_user" 表(部分版本可直接生效; 表不存在则跳过);
-- b) 用户在首选项里把 User 0 的 payload protocol 设为 "sue"(可靠路径,
--    见文件头"DLT_USER 配置")。
DissectorTable.get("wtap_encap"):add(147, p_sue)
pcall(function ()
  DissectorTable.get("dlt_user"):add(0, p_sue)
end)
-- 接入 2: EtherType(EN10MB pcap 中以太层后的 shim+payload; 偏好 shim0/shim1
-- 实时生效, 但 EtherType 值本身改动需重载插件——表注册在加载时绑定)。
-- 注: 不做 EN10MB 兜底 postdissector——带前导的数据帧/CtlOS 在原生 eth 下
-- 顶层是乱解析的 malformed 树, 混合展示不可读; 这类场景一律用 USER0(147)
-- 整帧自定义解析, CC_Update/PFC 等无前导真以太帧在 USER0 路径内按以太
-- 结构解析(带 MAC/Opcode 字段, 不会出现原生 "Unknown")。
local et = DissectorTable.get("ethertype")
et:add(0xFCF1, make_afh_entry("sueafh0", "SUE AFH (EtherType 0)", function () return p_sue.prefs.shim0 end))
et:add(0xFCF2, make_afh_entry("sueafh1", "SUE AFH (EtherType 1)", function () return p_sue.prefs.shim1 end))
