# 工程约定

- 使用原生 Windows、PowerShell 7；修改前查看 Git 状态，保留已有工作。
- 默认由用户核对界面。使用 Computer Use、通过它截图或模拟输入前，说明用途并取得明确授权。
- 启动子代理前取得用户授权。
- 测试任务读写必须使用独立的临时笔记，不修改实际 Obsidian 笔记。
- 不提交 `resources/config.json`、本机诊断、笔记、构建缓存或凭据；配置格式在 `resources/config.example.json`。
- 渲染修改运行原生测试；Markdown 和写回修改运行对应测试。合成图案验证不能代替用户的视觉验收。
- 未经明确要求，不提交、推送或发布。
