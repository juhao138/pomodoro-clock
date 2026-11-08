# 桌面时钟番茄钟

全屏覆盖的桌面时钟 + 番茄钟 + 待办清单。Windows 原生程序，单文件 290 KB，不依赖任何运行库。

![主界面](docs/screenshots/main.png)

## 特点

- **时钟是主角**：超大字号（雅黑粗体）、左侧对齐、`HH:MM:SS` 每秒刷新
- **番茄钟**：专注 25 / 短休 5 / 长休 15，每 4 个专注一次长休，阶段自动衔接
- **待办清单**：毛玻璃卡片，定时浮出（默认每 30 秒露 8 秒），也可按 L 常驻
- **独立编辑窗口**：按 E 打开，支持中文输入法，增删改、勾选完成
- **柔和提示音**：程序内合成的风铃音（E5-G5-C6 琶音），不依赖音频文件
- **全屏覆盖**：盖住任务栏并置顶；F11 可切成 980×640 小窗口当桌面挂件
- **透明度**：T 键在 100% / 91% / 78% / 66% 之间切换

## 截图

| 待办卡片 | 编辑窗口 |
|---|---|
| ![](docs/screenshots/main.png) | ![](docs/screenshots/editor.png) |

## 使用

双击 `桌面时钟番茄钟.exe` 即可。

### 按键

| 键 | 作用 | 备用键 |
|---|---|---|
| 空格 | 开始 / 暂停（点进度环也行） | |
| R | 重置回专注 25:00 | F4 |
| N | 跳过当前阶段 | F5 |
| L | 待办常驻 / 收起 | F3 |
| E | 打开待办编辑窗口 | F2 |
| T | 透明度循环 | F6 |
| ↑ ↓ | 选择待办条目 | |
| F11 | 全屏 ↔ 窗口 | |
| Esc | 退出 | |

> 中文输入法下字母键会被输入法当成拼音吃掉，所以主窗口已用 `ImmAssociateContext(hwnd, NULL)`
> 摘掉 IME 关联；万一你的输入法还是抢先，用 F2–F6 这套等价键位。

### 命令行

```
桌面时钟番茄钟.exe --work 50 --short 10 --long 30     # 自定义时长（分钟）
桌面时钟番茄钟.exe --list-every 30 --list-dur 8        # 待办浮出节奏
桌面时钟番茄钟.exe --todo-file D:\list.txt             # 指定清单文件
桌面时钟番茄钟.exe --window                            # 以窗口模式启动
桌面时钟番茄钟.exe --opacity 2                         # 启动时用第 2 档透明度
桌面时钟番茄钟.exe --mute                              # 不响提示音
桌面时钟番茄钟.exe --test-chime                        # 试听提示音
桌面时钟番茄钟.exe --font-check                        # 打印实际使用的字体
桌面时钟番茄钟.exe --selftest                          # 跑 26 项自测，不开窗口
```

## 自己编译

只需要 [Zig](https://ziglang.org/download/)（单文件编译器，免安装）：

```bat
python _build\make_clock_icon.py                       :: 生成 _build\clock.ico
python _build\make_res.py _build\clock.ico _build\clock.res

zig cc -O2 -std=c11 -Wl,--subsystem,windows -Wl,-e,mainCRTStartup ^
       -o 桌面时钟番茄钟.exe pomodoro_clock.c _build\clock.res ^
       -lgdiplus -lgdi32 -luser32 -lole32 -lcomctl32 -limm32 -lwinmm
```

`_build\make_res.py` 由 `clock.ico` 生成 Windows 资源文件（RT_ICON + RT_GROUP_ICON），
不需要 windres / llvm-rc。

## 实现要点

- **Win32 + GDI+**：圆角、弧线、文本全部抗锯齿；小字用 ClearType 并对齐整像素，
  大字用灰度抗锯齿（避免彩色毛边）
- **双缓冲**：画到 32 位 DIB Section 再 `BitBlt`，全屏 10fps 不闪
- **背景缓存**：斜向渐变 + 四团极光只渲染一次存成位图，每帧直接贴；叠层椭圆做柔光，
  避免 `PathGradient` 的 sigma 混合在部分环境失效导致的硬边圆盘
- **毛玻璃卡片**：圆角裁剪 + 白色竖向渐变（12%→4%）+ 1px 白色描边 + 顶部高光 + 淡投影
- **计时不漂移**：`WM_TIMER` 100ms 触发，但用 `GetTickCount64` 算真实 dt；
  一次 tick 跨过阶段终点时，多余秒数结转到下一阶段
- **分层窗口按需启用**：只有透明度 <100% 时才挂 `WS_EX_LAYERED`
  （分层窗口会让系统禁用 ClearType，小字会发虚）
- **清单存盘**：优先 exe 同目录（便携），不可写则退到 `%APPDATA%\PomodoroClock`

## 目录

```
pomodoro_clock.c        全部源码（约 1900 行，单文件）
_build/make_clock_icon.py   用 Pillow 画图标
_build/make_res.py          把 .ico 打包成 .res 资源
docs/使用说明.txt           面向使用者的中文说明
docs/screenshots/           截图
```

## 已知限制

- 只覆盖主显示器
- 全屏是普通置顶窗口，不阻止系统休眠
- 编辑窗口用系统 LISTBOX（条目自绘），滚动条是自绘的
- 清单上限 128 条，单条 160 字

## 贡献者

本项目由人和 AI 协作完成，详见 [CONTRIBUTORS.md](CONTRIBUTORS.md)。

| 贡献者 | 角色 |
|---|---|
| [@juhao138](https://github.com/juhao138) | 需求、设计方向、实机验证与反复打磨 |
| **DeepSeek** | 代码实现、调试、自测用例 |

## License

MIT
