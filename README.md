# CodexTaskbar

一个面向 Windows 10 的极简 Codex 任务栏状态小工具。

## v0.2

- Codex Desktop 打开时显示，关闭后自动隐藏数据区，仅保留托盘图标。
- 优先放在左侧竖向任务栏的 TrafficMonitor 上方。
- 黑底白字、紧凑显示，字号 8–24 pt 可直接输入。
- 每一项都可以独立勾选显示：
  - 5 小时剩余百分比
  - 5 小时恢复时间
  - 7 天剩余百分比
  - 绑定会话 Context 已用百分比
  - 绑定会话 Cache 命中率
  - 本轮代码改动统计
- 默认布局示例：
  - 第一行：`13%19:38`
  - 第二行：`86% 66%`（若启用 Cache：`86% 66% 98%`）
  - 第三行仅在本轮实际改代码后出现：`32 1026 265`，新增为绿色、删除为红色。
- 鼠标悬浮任务栏区域时显示带标签的完整信息。
- 会话来源支持：
  - 手动绑定 Thread ID（推荐、默认）
  - 自动跟随活动任务（实验）

## 数据来源与隐私

- 5h / 7d 配额：官方 Codex `app-server` 的 `account/rateLimits/read`。
- Context / Cache / 本轮代码改动：只读订阅 Codex Desktop 本地 IPC `\\.\pipe\codex-ipc`。
- 会话 follower 只订阅状态，不会 `resume` 会话、发送 prompt、抢占 owner 或修改 Codex。
- 不扫描 Git 来统计代码改动；第三行来自 Codex 当前 turn 自身的 unified diff。

程序不会：

- 读取或保存聊天正文；
- 建立统计数据库；
- 读取或复制 `auth.json`；
- 注入或修改 Codex Desktop。

## 构建

需要 Windows + Visual Studio C++ Build Tools / MSVC：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成：`build\Release\CodexTaskbar.exe`
