# CLAUDE.md

本项目名为“SUE Scale-Up Ethernet 仿真平台”，当前本地扩展版本为 v2。SUE 路径包含 SUE Gen1 Lite + LLR + CBFC + PFC；基座另保留 RoCE/RDMA 等能力，当前 AI harness 只接通 SUE，详见 README.md。

- **设计基准**:`DESIGN.md`(代码实现/评审/验证的唯一基准,必读)。
- **项目指南与版本管理规则**:见 `AGENTS.md`。
- **参考材料**:`reference/`(第三方,gitignored):SimAI/SUE/UEC + `ns-3.48.tar.bz2`。
- **受控资产**:`DESIGN.md`、`src/sue/`;变更须评审后提交,严禁直接覆盖或删除,误删用 git 回溯。
- **原则**:新增独立 `sue` 模块,不修改现有定制模块;小步验证(S0–S5)。
