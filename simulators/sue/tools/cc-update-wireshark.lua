-- cc-update-wireshark.lua — SUE CC_Update 独立解析脚本(Wireshark Lua postdissector)
--
-- 用途:只想在普通 EN10MB pcap 里看 UEC CC_Update(EtherType 0x8808 +
-- Opcode 0xFFFE)字段、不想装整套 SUE 插件/配 DLT_USER 时,单装本文件即可:
-- 复制到 Wireshark 个人插件目录(Linux ~/.local/lib/wireshark/plugins/,
-- Windows %APPDATA%\Wireshark\plugins\)重启。零配置,对非 CC 帧零开销。
--
-- 解析字段(前缀 suecc.*,与主插件 sue.* 互不冲突、可共存):
--   CID(3B) / MsgType(1B) / 16x20bit 逐 VC CC 计数(40B MSB-first 连续
--   位打包)/ FCS(CRC-32)。线布局与仿真 SueCbfcCcHeader::Serialize 对齐。
--
-- 实现:postdissector(原生 eth/macctrl 解完后追加树)——Wireshark 的
-- MAC Control 无 opcode 子表,0xFFFE 原生显示 Unknown,只能旁挂。
-- USER0 pcap 已由主插件解析为 SUE-CC 时自动跳过,避免重复树。
-- 仅用 Lua 5.2 兼容语法(无位运算符)。

local p_cc = Proto("suecc", "SUE CC_Update (standalone)")

local f = {
  cid     = ProtoField.uint32("suecc.cid",     "CID", base.HEX),
  msgtype = ProtoField.uint8("suecc.msgtype",  "MsgType", base.HEX, { [1] = "VC[0:15]" }),
  vc      = ProtoField.uint32("suecc.vc",      "CC (consumed-credit counter)", base.DEC),
  fcs     = ProtoField.uint32("suecc.fcs",     "FCS (CRC-32)", base.HEX),
}
p_cc.fields = f

local function shr(v, n) return math.floor(v / (2 ^ n)) end

-- 16x20bit 连续 MSB-first 打包(40B 域起点 base),取第 idx 个(idx 0..15)。
-- 20bit 字段只落在 4bit 对齐边界(shift 只会是 0 或 4),3 字节读即可覆盖。
local function cc_value(tvb, base, idx)
  local bitpos = idx * 20
  local byte = base + math.floor(bitpos / 8)
  local shift = bitpos % 8
  return math.floor(shr(tvb(byte, 3):uint(), 4 - shift) % 0x100000)
end

p_cc.dissector = function (tvb, pinfo, tree)
  -- 主插件在场则整体让位(其 USER0 整帧路径已覆盖 CC_Update, 且带前导/CtlOS
  -- 的场景应整包用 USER0 抓包, 不该走到本脚本的混合模式)。运行时检查而非
  -- 加载时, 与两脚本的加载顺序无关。
  if SUE_PLUGIN_LOADED then return end
  if tvb:len() < 64 then return end
  if tvb(12, 2):uint() ~= 0x8808 then return end      -- MAC Control
  if tvb(14, 2):uint() ~= 0xFFFE then return end      -- CC_Update opcode
  -- 主插件已解析(USER0 整帧 / EN10MB 兜底)则让位: 主插件以全局表按帧号打标
  -- (所有 Lua 脚本共享同一状态, 不依赖版本相关的 pinfo 私有字段)
  if _SUE_CC_DONE and _SUE_CC_DONE[pinfo.number] then return end

  local st = tree:add(p_cc, tvb(), "SUE CC_Update (UEC §5.2.6.2)")
  st:add(f.cid, tvb(16, 3), tvb(16, 3):uint())
  st:add(f.msgtype, tvb(19, 1))
  local parts = {}
  for i = 0, 15 do
    local v = cc_value(tvb, 20, i)
    local it = st:add(f.vc, tvb(20, 40), v)
    it:append_text(" (VC" .. i .. ")")
    if v > 0 then parts[#parts + 1] = "vc" .. i .. "=" .. v end
  end
  st:add(f.fcs, tvb(60, 4))
  -- 覆盖原生 macctrl 的 "Unknown": 解 fence + 清列后重设(postdissector 场景)
  pcall(function ()
    pinfo.cols.info:clear_fence ()
    pinfo.cols.info:clear ()
    pinfo.cols.info = "CC_Update " .. (next(parts) and table.concat(parts, ",") or "all-zero")
  end)
end

register_postdissector(p_cc)
