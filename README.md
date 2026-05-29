# Taskbar Current Monitor Apps

Windhawk mod：多显示器连接时，让不同屏幕上的任务栏只显示当前屏幕上的运行应用，同时保留任务栏固定项目。

## 功能

- 每个显示器任务栏只显示该显示器上的运行窗口按钮。
- 排除任务栏固定项目：mod 不删除、不隐藏、不重排固定项目。
- 固定项目继续由 Explorer 原生逻辑显示，目标是让固定项目在所有任务栏都可见。
- 启用时自动写入多显示器任务栏设置。
- 关闭 / 卸载 mod 时默认恢复启用前的任务栏状态。

## 文件

- `taskbar-current-monitor-apps.wh.cpp`：Windhawk 插件源码。

## 安装

1. 打开 Windhawk。
2. 新建 mod，或选择从本地代码安装。
3. 导入 `taskbar-current-monitor-apps.wh.cpp`。
4. 编译并启用。
5. 如果首次启用或关闭后没有立刻变化，重启 `explorer.exe` 或注销重登。

## 实现说明

插件没有硬改任务栏内部按钮列表，而是强制 Explorer 使用 Windows 自带的多显示器任务栏模式：

```text
MMTaskbarEnabled = 1
MMTaskbarMode    = 2
```

`MMTaskbarMode = 2` 的目标是让运行窗口按钮只出现在窗口所在显示器的任务栏上。固定项目不在 mod 的过滤范围内，避免误伤用户已经固定到任务栏的应用。

## 设置

- `keepEnforced`：保持强制模式，防止 Explorer 读回旧值。
- `writeRegistry`：同时写入注册表，默认开启。
- `restoreOnUnload`：关闭 / 卸载 mod 时恢复启用前状态，默认开启。
