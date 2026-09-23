# glm_probe — GLM Coding Plan 额度端点真机探针

`probe_usage.ps1`：用与 QuotaBar 完全相同的方式调用智谱
`open.bigmodel.cn/api/monitor/usage/quota/limit`，把响应 JSON 原样导出到本目录
`glm_response.json`，供 `ParseGlmResponse` 契约锁定/字段漂移排查用。

- 密钥来源：`%LOCALAPPDATA%\QuotaBar\secrets.dat`（DPAPI CurrentUser 解密，取第 3 段
  GLM blob），只在内存中，只发往 bigmodel.cn 本身，不打印、不落盘。
- 请求形态：GET，`Authorization: <裸Key>`（**无 Bearer 前缀**，与监控端点约定一致）。
- 2026-09-12 真机样本即由本探针导出（首版按社区旧形态实现解析失败，靠它定位到
  CREDIT_LIMIT 新契约）。复跑：`powershell -NoProfile -ExecutionPolicy Bypass -File probe_usage.ps1`
