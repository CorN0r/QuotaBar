# 原厂供应商图标来源

`assets/provider/*.png`（128px 圆角芯片）由 `make_provider_icons.py` 从 `provider_raw/`
的官网素材归一化生成；嵌入 exe 的 RCDATA 资源名 PVID_KIMI / PVID_CODEX / PVID_GLM /
PVID_DEEPSEEK（见 assets/quota_bar.rc）。图标版权归各厂商所有，仅用于标识对应服务。

| 类型 | 原始来源 | 处理 |
| --- | --- | --- |
| Kimi | www.kimi.com/favicon-light.ico（48px，黑底白 K + 蓝点） | 放大铺满、圆角裁边 |
| Codex (OpenAI) | openai.com/apple-touch-icon.png（180px，白底黑结环） | 按结环包围盒裁切、反白，**透明底无芯片**，占画布 ~98% |
| GLM (智谱) | bigmodel.cn/static/images/favicon.png（90px，深色圆角章白 Z） | 放大、圆角 |
| DeepSeek | www.deepseek.com/favicon.ico（225px 透明底蓝鲸） | 贴白芯片 |

各尺寸显示（嵌入条名称行 ~17px、卡片徽章 30dip、圆环徽章 20dip）均由运行时高质量
插值缩放。重新生成：`python make_provider_icons.py`（需 Pillow）。
