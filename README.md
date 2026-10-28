# CodexTaskbar

一个面向 Windows 10 的极简 Codex 配额任务栏小工具。

## v0.1 目标

- Codex Desktop 打开时，在任务栏显示两行：
  - 第一行：`5 小时剩余百分比 + 恢复时间`，例如 `89% 16:19`
  - 第二行：`7 天剩余百分比`，例如 `17%`
- Codex Desktop 关闭后自动隐藏数据区，只保留系统托盘图标。
- 优先放在左侧竖向任务栏的 TrafficMonitor 任务栏窗口上方。
- 黑色背景、白色 Segoe UI Semibold，紧凑显示。
- 右键菜单：设置、开机自动启动、重新连接、关于、退出。
- 字号可以直接输入数字，当前范围 8–24 pt。

## 数据来源

额度通过官方 Codex `app-server` 的 `account/rateLimits/read` 获取。

程序不会：

- 读取聊天正文；
- 扫描 session / rollout JSONL；
- 读取或复制 `auth.json`；
- 建立本地统计数据库；
- 注入或修改 Codex Desktop。

仅当 Codex Desktop 正在运行时，CodexTaskbar 才会启动一个隐藏的 `codex app-server` 子进程读取配额；关闭 Codex 后该子进程也会结束。

## 构建

需要 Windows + Visual Studio C++ Build Tools / MSVC：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成：

`build\Release\CodexTaskbar.exe`

GitHub Actions 会在 PR 和 main push 上自动构建 Windows x64 版本。
