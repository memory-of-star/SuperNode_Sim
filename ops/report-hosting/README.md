# SUE v2 报告公网服务

实际运维目录：`ops/report-hosting/`；HTTP根目录：`results/sue/v2-topology-kimi-glm53/`。原`.report-hosting`路径是兼容软链接。

当前公网域名：https://trades-joan-athletes-finals.trycloudflare.com

- [多Die MoE验收](https://trades-joan-athletes-finals.trycloudflare.com/moe-striped-validation/report.html)
- [硬件探索历史报告](https://trades-joan-athletes-finals.trycloudflare.com/hardware-inference-20261022/report.html)
- [原拓扑报告](https://trades-joan-athletes-finals.trycloudflare.com/report.html)

2026-10-22迁移后，本地及公网多Die报告均返回200，SHA256与磁盘文件一致。详细验证见`deployment.json`。URL中的报告路径不变，临时域名因依赖服务重启已经改变；旧域名不保证可访问。

服务由用户级systemd管理。HTTP源站仅监听127.0.0.1:18765，只提供上述报告子树和显式指定的报告ZIP，禁止目录列表和越界路径。临时隧道不是固定公网IP或永久托管服务，重启可能再次换域名。

```bash
systemctl --user status sue-v2-report-http.service sue-v2-report-tunnel.service
rg 'https://.*trycloudflare.com' ops/report-hosting/tunnel.log | tail -1
```

配置在`~/.config/systemd/user/`，已经指向新的运维路径；迁移前副本存于`archive/service-units/`。重启HTTP服务会影响依赖它的隧道，请在重启后更新部署记录与共享链接。
