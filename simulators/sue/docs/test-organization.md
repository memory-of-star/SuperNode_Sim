# 测试组织规范(2026-08-13 重构)

## 定位分工

| 层 | 用途 | 载体 | 入口 |
|---|---|---|---|
| **scratch 单测** | 每模块每功能点一个可执行程序,自检式 PASS/FAIL 判语;规格对照(UEC 条款逐点验证)也在此层 | `scratch/sue-<模块>-<功能点>.cc` | `./run-scratch-checks.sh` |
| **TestSuite 集成测试** | 持续集成:同构用例的权威版本,`NS3_TESTS=ON` 下 `libsue-test` 一键全跑,防回归 | `src/sue/test/sue-test.cc`(单文件;按模块分节) | `./run-sue-tests.sh` |
| **等价/性能** | MTP 等价、吞吐、incast 等场景级验证 | scratch(sue-mtp-*/sue-thpt-*/sue-incast-*) | 对应 run-*.sh |

原则:
1. **同一功能点的单测只保留一份权威实现**——TestSuite 与 scratch 不再同构复制(已漂移过);新功能点按"协议状态机/纯引擎逻辑 → TestSuite;设备/网络路径 → scratch"分流,不双写。
2. **命名 = 模块 + 功能点**(kebab-case),不再用 S0–S8 阶段编号(阶段只出现在 DESIGN §11 里程碑里)。
3. TestSuite 用例名同样带模块前缀,且**不含 `/`、`§` 等 runner 禁字符**(现 2 个用例名触发 "Invalid test name" 告警,一并修)。

## 模块划分

| 模块 | 范围 | scratch 前缀 |
|---|---|---|
| `afh` | AFH 转发:帧格式、shim、LPM/ECMP、TTL、wire-time/IFG、交换机 | sue-afh-* |
| `llr` | LLR:状态机、preamble、INIT/INIT_ECHO、NACK/REPLAY、FLUSH、定时器、计数器、PCS 闪断、重放缓冲 | sue-llr-* |
| `cbfc` | CBFC:信用记账、门控、CF/CC_Update、泄漏回收、环回防护 | sue-cbfc-* |
| `pfc` | PFC:阈值/共享池/迟滞、pause/resume、802.1Qbb 帧 | sue-pfc-* |
| `fc` | FC 门面与共存:FC_MODE、nine rules、帧头 | sue-fc-* |
| `cfg` | 配置:SueReadConf/SueSetGlobal/ApplySueConfig/conf 预置 | sue-cfg-* |
| `entry` | 前端入口:拓扑解析/校验、BFS+ECMP 路由、端点绑定 | sue-entry-* |
| `endpoint` | 主机侧生成器:SueLiteEndpoint | sue-endpoint-* |
| `mtp` | MTP:驻留断言、等价性、契约 | sue-mtp-* |
| `thpt` | 吞吐/性能场景 | sue-thpt-* |
| `incast` | 拥塞场景(2:1、多 VC) | sue-incast-* |
| `scale` | scaleup 拓扑:8 卡机内置 switch、超节点互联、规模冒烟 | sue-scale-* |
| `vrf` | VRF 平面转发:§12.40 分段语义的场景级验证(32 平面、纯 XPU ID 寻址、同口通信 all-to-all,§12.41) | sue-vrf-* |

## 现有 scratch 迁移表(旧 → 新)

| 旧名 | 新名 |
|---|---|
| sue-s0-check | sue-afh-device-dispatch |
| sue-s1-parse | sue-afh-header-parse |
| sue-s1-fwd | sue-afh-forward |
| sue-s2-cbfc-engine | sue-cbfc-engine-credit |
| sue-s2-cbfc-e2e | sue-cbfc-e2e-gating |
| sue-s2-timing | sue-afh-wire-time |
| sue-s3-engine | sue-llr-engine |
| sue-s3-flush | sue-llr-flush-init |
| sue-s3-llr | sue-llr-preamble-fcs |
| sue-s3-poisoned | sue-llr-poisoned |
| sue-s3-wake | sue-llr-timer-wakeup |
| sue-s4-ccupdate | sue-cbfc-cc-update-onwire |
| sue-s4-coexist | sue-fc-llr-cbfc-coexist |
| sue-s4-counters | sue-fc-counters |
| sue-s4-fcctl | sue-fc-facade |
| sue-s4b-pfc | sue-pfc-gating |
| sue-s5-ctl-frames | sue-fc-ctl-onwire |
| sue-s5-headers | sue-fc-frame-headers |
| sue-s5-pcs-lost | sue-llr-pcs-lost |
| sue-s5b-endpoint | sue-endpoint-ring |
| sue-s5c-thpt | sue-thpt-line-rate |
| sue-s5e-incast | sue-incast-2to1 |
| sue-s6-config | sue-cfg-parse-apply |
| sue-s6-entry | sue-entry-dispatch |
| sue-s7-replay-buf-rtt | sue-llr-replay-buffer |
| sue-s8-mtp | sue-mtp-equivalence |
| sue-e1-conf-smoke | sue-cfg-preset-smoke |

迁移方式:`git mv` 保历史;文件内 verdict 字符串同步改(如 `S0 CHECK PASS` → `AFH DEVICE-DISPATCH PASS`);`run-scratch-checks.sh` 更新;AGENTS.md/DESIGN 引用更新。

## TestSuite 用例命名(模块前缀)

现有用例按模块重命名(内部按模块分节):
- `AFH <feature>`(instantiable / attach / dispatch / header-parse / routing-ecmp / forwarding / counters / wire-time)
- `LLR <feature>`(preamble-fcs / nack-retransmit / flush-init / flush-stays-flush / partial-ack-wake / replay-during-pfc-pause / poisoned / init-handshake / modes-behaviors / vld / init-echo-seq / ineligible-preamble / ack-period / acked-seq / pcs-lost / status / replay-ct-unlimited / can-tx-limits / replay-buffer-gates)
- `CBFC <feature>`(engine-credit / e2e-gating / ttl-drop-refund / lost-credits-guard / cc-update-onwire / counters)
- `PFC <feature>`(header / gating / shared-pool / facade-onrx)
- `FC <feature>`(facade / coexist / rule3 / rule9 / llr-window-driven)
- `CFG <feature>`(default-100g-e2e / vc-map)
- `ENDPOINT generator`

## CI 流程(新增 `run-ci.sh` 一键)

```
./build.sh && ./run-ci.sh
# = ./run-sue-tests.sh(TestSuite,NS3_TESTS=ON 定向构建)
# + ./run-scratch-checks.sh(全部单测)
# + ./run-s5b.sh / ./run-s8-mtp.sh 2,4,8(MTP 等价)
# + ./run-s5c-thpt.sh / ./run-s5e-incast.sh(性能场景)
```

## UEC spec 功能点

所有"可仿真"的 UEC 条款拆解与实现/验证状态见 `docs/uec-spec-matrix.md`——单测文件命名与该矩阵的条目一一对应(如 `sue-llr-nack-retransmit` = LLR-SP-xx)。
