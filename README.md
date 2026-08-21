# openDock

**Windows 平台上的 macOS 风格 Dock 栏** —— C++17 + DirectComposition 原生实现，零第三方依赖。

openDock 在屏幕的四条边上提供可同时存在的悬浮启动栏：图标随鼠标做鱼眼放大、离开自动隐藏于屏幕边缘、支持拖入添加与拖出删除，配置持久化为单个 `config.json`。整个渲染管线基于 DirectComposition + Direct2D 的零重绘合成路径，空闲与被遮挡时 CPU 占用归零。

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **四边同时停靠** | 上/下/左/右四条边各自持有一个独立的 Dock 实例，拥有各自的图标集、放大动画、自动隐藏与拖放，由单一消息循环统一编排 |
| **鱼眼放大** | macOS 风格的余弦衰减邻域联动放大，放大方向恒朝屏幕内侧；可按边单独开关 |
| **弹簧物理动画** | 半隐式欧拉积分的二阶弹簧系统，驱动 scale / offset / opacity 三个维度，支持悬停、弹跳、入场、复位四套参数 |
| **边缘自动隐藏** | 隐藏于屏幕边缘，鼠标靠近感应区自动弹出；显示/隐藏延迟均可配置 |
| **空闲鼠标穿透** | 基于 `WM_NCHITTEST` 的按区域命中判定（非全局 `WS_EX_TRANSPARENT`），空闲时留白区穿透、Dock 条始终可交互 |
| **遮挡挂起** | 被其它窗口完全遮挡时事件驱动地挂起看门狗并释放合成资源，解除遮挡后自动恢复 |
| **拖入添加 / 拖出删除** | OLE 拖放（STA 公寓 + `IDropTarget`）从资源管理器拖入文件或文件夹即添加；拖动图标离开本边感应区即删除 |
| **图标自动提取** | 从 EXE/DLL 提取图标并在内存中编码为 PNG（不落盘）；缺失文件回退系统默认文件类型图标，绝不产生灰色占位 |
| **配置持久化** | 去抖落盘（800ms）+ 临时文件原子替换，杜绝写入中断导致的配置损坏 |
| **开机自启动** | 优先创建计划任务（logon 触发器，秒级拉起）——**首次须以管理员权限运行并勾选自启**，否则回退 `HKCU\...\Run`（有约 1 分钟错峰）；启动期按真值表对齐「实际状态」与「配置意图」 |
| **毛玻璃与圆角** | Acrylic / Accent Blur / DwmBlurBehind 三级降级；Win11 下启用 DWM 圆角 |
| **多显示器与 DPI** | Per-Monitor DPI Aware V2，响应 `WM_DPICHANGED` / `WM_DISPLAYCHANGE` 自动重定位 |
| **GDI 降级** | DirectComposition 不可用时回退到 GDI + Layered Window 软件合成路径 |
| **系统托盘** | 常驻托盘入口（图标与 exe 应用图标同源，均取内嵌 `MAINICON`），提供「显示边 / 图层位置 / 开机自动启动 / 退出」 |

---

## 系统要求

### 运行环境

- **Windows 10 1703 及以上**（x64）。
  - 依赖 `SetProcessDpiAwarenessContext`（Win10 1703+）、`GetDpiForWindow`（Win10 1607+）；两者均为运行时 `GetProcAddress` 动态探测，缺失时优雅降级到 96 DPI。
  - DirectComposition 与 Direct2D 为 Win8+ 能力；不可用时自动走 GDI 回退路径。
  - 窗口圆角（`DWMWA_WINDOW_CORNER_PREFERENCE`）仅 Win11 生效，Win10 下调用失败即忽略。
- 若目标机器未安装 VC++ 运行库，需随程序分发 `msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll` / `ucrtbase.dll`。

### 构建环境

| 组件 | 要求 | 来源 |
|------|------|------|
| CMake | **≥ 3.20** | `CMakeLists.txt: cmake_minimum_required(VERSION 3.20)`；构建脚本默认使用 VS 内置的 CMake |
| C++ 标准 | **C++17**（`CXX_EXTENSIONS OFF`） | `CMakeLists.txt` |
| 编译器 | **MSVC**（`cl.exe`，Host x64 / Target x64） | `msvc_env.sh` 固定使用 `bin/Hostx64/x64` |
| 生成器 | **Ninja** | `build.sh` 显式 `-G Ninja`，使用 VS 内置的 `ninja.exe` |
| Windows SDK | 任一版本，需包含 `Include/<版本>/ucrt` | `msvc_env.sh` 自动取版本号最大者 |
| Shell | **Bash**（Git Bash / MSYS2 / WSL 均可） | 构建链路为 `.sh` 脚本 |

> **注意**：本工程仅支持 **x64**。`msvc_env.sh` 中的工具链路径、`INCLUDE` / `LIB` 均硬编码为 x64，未提供 x86 或 ARM64 配置。

链接的系统库：`d2d1` `dwrite` `dcomp` `d3d11` `dxgi` `windowscodecs` `dwmapi` `shell32` `ole32` `user32` `gdi32` `advapi32` `uuid` —— 全部随 Windows SDK 提供，**无需任何第三方依赖**（JSON 解析亦为手写实现）。

---

## 快速开始

### 1. 准备环境

构建脚本会自行探测 Visual Studio 与 Windows SDK，**无需手动运行 `vcvarsall.bat`**。探测顺序由 `msvc_env.sh` 定义：

1. **VS 安装根**：`vswhere.exe -latest` → 环境变量 `$VSINSTALLDIR` → 遍历 `C:/`、`D:/` 下的 `Program Files/Microsoft Visual Studio/*/*`
2. **MSVC 工具链版本**：`<VS>/VC/Tools/MSVC/` 下按版本号取最新（`sort -V`）
3. **Windows SDK**：`$WINDOWSSDKDIR` → 默认 `<系统盘>/Program Files (x86)/Windows Kits/10`，版本取 `Include/` 下最新

任一步失败都会打印明确的错误原因与绕过方式，并中止构建（绝不静默继续）。如需手动指定：

```bash
export VSINSTALLDIR='C:\Program Files\Microsoft Visual Studio\2022\Community'
export WINDOWSSDKDIR='C:\Program Files (x86)\Windows Kits\10'
```

### 2. 构建

工程只提供**单一构建形态**：Release 优化 + Windows GUI（无控制台）子系统。源码改动后**必须重新构建**才能生效（`release/openDock.exe` 是陈旧二进制，见下方说明）。

#### 方式一：build.sh（推荐，跨 Shell）

在 **Git Bash / MSYS2 / WSL** 中进入项目根目录执行：

```bash
./build.sh                # 全新 / 增量构建到 build/
./build.sh --no-config    # 仅增量 build，跳过 configure
./build.sh --clean        # 先删除构建目录再全新构建（pristine）
./build.sh --help         # 查看内置帮助
```

构建产物：`build/openDock.exe`。

> **图标核验**：构建后运行 `python tools/pe_icon_res.py build/openDock.exe` 可确认 exe 已内嵌应用图标（`RT_GROUP_ICON`），退出码 0=已嵌入 / 1=未嵌入。

#### 方式二：build.bat（Windows cmd，无需 Bash）

**双击 `build.bat`**，或在项目目录打开 cmd 运行 `build.bat`。除完成与 `build.sh` 等价的构建外，还会把可直接运行的完整分发包打包进 `release/`：

- `openDock.exe` + `res/config.json` + `res/` 资源
- MSVC CRT / UCRT 运行库 dll（保证无 VS 的干净机器也能运行）

```text
build.bat            # Release 构建并打包 release/
build.bat --clean    # 先清 build/ 再全新构建
build.bat --no-config
```

> **重要**：`release/openDock.exe` 不会随 `build.sh` 自动更新，必须经 `build.bat` 打包、或手动把 `build/openDock.exe` 复制覆盖。要验证最新源码改动，请以 `release/` 内的 exe（或 `build/openDock.exe`）为准，不要直接运行历史 `release/` 二进制。

### 3. 运行

```bash
cd build
./openDock.exe
```

启动后 Dock 默认隐藏于屏幕边缘（`autoHide` 默认开启），将鼠标移向已启用的屏幕边缘即可唤出。**退出方式：右键点击系统托盘图标 → 「退出 openDock」。**

> **⚠️ 开机快速启动的前提：以管理员权限运行过一次**
>
> openDock 通过「计划任务（logon 触发器）」实现开机**秒级**启动，但**标准用户没有创建计划任务的权限**（`schtasks` 会被拒绝访问）。因此：
>
> 1. **右键 `openDock.exe` → 「以管理员身份运行」**；
> 2. 在托盘菜单勾选「开机自动启动」——此时计划任务创建成功；
> 3. 之后每次开机登录都由 Task Scheduler 秒级触发，**无需**再以管理员运行。
>
> 若从未以管理员运行过，自启会**静默回退**到 `HKCU\...\Run`，而 Windows 对 Run 键启动项存在**约 1 分钟错峰延迟**（`StartupDelayInMSec` 在 Win11 上实测无效）——表现为「开机后约 1 分钟 Dock 才出现」。

CMake 在 configure 阶段会把 `res/config.json` 复制到 `build/config.json` 与 `build/res/config.json`（兼容双击 exe 时的两种候选解析路径），并通过 `copy_dock_icons` 这一 POST_BUILD 目标在每次增量构建时同步 `res/icons/` —— 修改图标资源后无需清缓存即可生效。

**应用图标与托盘图标同源**：`app.rc` 声明的 `MAINICON`（`res/icons/tray_icon.ico`，由 `tools/gen_app_icon.py` 从 `res/icons/tray_icon.png` 生成 256–16 多尺寸）在编译期嵌入 exe，作为文件资源管理器 / 任务栏 / Alt-Tab 的应用图标；托盘图标运行时从同一内嵌资源加载（先按字符串名 `MAINICON`，失败回退整数 ID 1）。两者 100% 同源，且单 exe 分发不再依赖随行的 `tray_icon.png`。

可用命令行开关：

| 开关 | 作用 |
|------|------|
| *(无)* | 正常交互模式：`DockManager` 为每条启用的边创建 Dock 并运行消息循环 |
| `--force-gdi` | 强制走 GDI 回退渲染路径（排查渲染问题时用） |
| `--autostart` | 标记「本次为开机拉起」（计划任务 / Run 键均以此参数拉起），延迟 2s 再建窗口以避开 explorer 未就绪 |

---

## 质量与验证说明

- **无自动化测试 / 验证套件**：`CMakeLists.txt` 中原有的 `test_*` 目标与 `--verify` / `--acceptance` 无头验证套件均已移除，工程为纯应用构建。**没有 ctest / `--target test` 可用**。
- 渲染、命中、动画等依赖真实窗口与鼠标手势的行为**无法在无头环境中验证**（无头环境永远 96 DPI、无真实指针手势，命中缝隙类缺陷必漏）。质量需在本机 GUI 中人工回归：启动后逐边唤出、拖入/拖出、托盘菜单、自启动等核心路径。
- 二进制版本可通过构建戳核对：`OPENDOCK_BUILD_TIME` 与 `OPENDOCK_BUILD_HASH`（`git rev-parse --short HEAD`，取不到为 `unknown`）由 CMake 注入，用于真机复测时一眼确认二进制版本，消解「陈旧二进制」歧义。
- 图标内嵌可用 `tools/pe_icon_res.py` 静态核验（无需启动 GUI）：`python tools/pe_icon_res.py <exe>`，退出码 0=已内嵌 `RT_GROUP_ICON` / 1=未内嵌。
- 工程在**正常运行时不产生任何日志文件**；早期版本的 `DiagLog` 文件日志与 `DebugExporter` JSON 快照机制已彻底移除。

---

## 架构概览

openDock 采用分层架构，依赖方向自上而下单向：**`app` 编排层 → `core` 纯逻辑层 + `render` 渲染层 + `platform` 系统封装层**；`utils` 提供横切工具。其中 `core/` 是**纯数学与状态**，不触碰任何 Win32 API，可独立运行。

```
                          ┌──────────────────────────────┐
                          │        DockManager            │
                          │   (app · 编排层)              │
                          │ 单消息循环 · 单托盘图标        │
                          │ 遮挡检测 · 统一去抖持久化      │
                          └──────────┬───────────┬────────┘
                ┌────────────────────┘           └────────────────────┐
        ┌───────▼────────┐                            ┌────────▼────────┐
        │   DockEngine   │  每条启用的边一个实例        │   DockEngine    │
        │ (app · 单边)   │  ─ friend ─►              │  (app · 单边)   │
        │ DockStateMachine│ DockInteraction          │  IconSetManager │
        │ IconSetManager │ IconSetManager            │                 │
        └───┬───────┬────┘                            └────────┬────────┘
            │       │                                         │
   ┌────────┼───────┼─────── 依赖与协作 ──────────────────────┼────────┐
   │        │       │                                         │        │
┌──▼─────┐ ┌▼───────▼──┐  ┌─────────────────▼────────┐  ┌─────▼──────┐
│ core/  │ │ render/   │  │      platform/           │  │  utils/    │
│ 几何   │ │RenderManager│  │ WindowManager           │  │ConfigManager│
│ 物理   │ │DirectComp  │  │ AutoStart (任务/注册表) │  │            │
│ 状态机 │ │Direct2D    │  │ TrayIcon               │  │            │
│零Win32 │ │D3D11 / WIC │  │ IconProvider           │  │            │
│ 依赖   │ │GDI_Fallback│  │ MouseHook / EventHook  │  │            │
└────────┘ └────────────┘  └────────────────────────┘  └────────────┘
```

### 各层职责

- **`app/`**：编排层。`DockManager` 为每条启用边创建并持有 `DockEngine`，运行唯一消息循环、维护唯一托盘图标、做遮挡检测与统一持久化；`DockEngine` 是单边控制器，按 `DockPosition` 索引，内部 `friend` 协作三个子模块——状态机 `DockStateMachine`、图标集合 `IconSetManager`、交互 `DockInteraction`。
- **`core/`**：纯逻辑层。`EdgeGeometry<Orient, RestAtFarEdge, InwardSign>` 以编译期模板表达四边几何，运行时唯一的位置分支在 `MakeGeometry(pos)`；另含 `DockState` 状态枚举、`SpringPhysics` 二阶弹簧积分、`DockConstants`。**无任何 Win32 依赖**。
- **`render/`**：渲染层。`RenderManager` 封装两种渲染态——窗口化（DirectComposition Visual 树，零重绘合成）、GDI 回退（Layered Window 软件合成）；并负责 Tooltip 与阴影效果。底层组合 DirectComposition（D3D11/DXGI）、Direct2D、DirectWrite、WIC。
- **`platform/`**：Win32 封装层。`WindowManager` 负责 `WS_POPUP` 分层透明窗口、毛玻璃三级降级、Win11 圆角、DPI/多显示器与 Z 序；`AutoStart` 管理开机自启动（计划任务优先，注册表回退）；`TrayIcon` 托盘；`IconProvider` 从 EXE/DLL 提取图标；`MouseHook` / `EventHook` 做穿透命中与遮挡检测。
- **`utils/`**：横切工具。`ConfigManager` 手写 JSON 解析与去抖原子落盘。

---

## 目录结构

```
openDock/
├── CMakeLists.txt            # 工程定义、链接库、构建戳注入、app.res 强制链接
├── app.rc                    # 应用图标资源（MAINICON，编译期嵌入 exe）
├── build.sh                  # 主推构建入口（--no-config / --clean / --help）
├── build.bat                 # 可选 / 遗留的 Windows 原生（cmd）构建入口，与 build.sh 等价
├── msvc_env.sh               # 探测 VS + Windows SDK，导出 MSVC 编译环境（x64）
├── src/
│   ├── Common.h              # 枚举(DockState/DockPosition)、常量、错误宏 DOCK_HR_CHECK
│   ├── main.cpp              # 入口（wWinMain，GUI 子系统）
│   ├── core/                 # 纯数学与状态，零 Win32 依赖
│   │   ├── EdgeGeometry.h        # 四边几何（编译期模板）
│   │   ├── DockState.h / .cpp    # 状态机
│   │   ├── SpringPhysics.h / .cpp# 二阶弹簧积分
│   │   └── DockConstants.h
│   ├── app/                  # 编排层
│   │   ├── DockManager.h / .cpp  # 多 Dock 编排、托盘、遮挡、统一持久化
│   │   ├── DockEngine.h / .cpp   # 单边控制器
│   │   ├── DockInteraction.cpp   # 命中、拖放、删除
│   │   ├── IconSetManager.*      # 图标集合管理
│   │   ├── ConfigManager.h / .cpp# 配置加载 / 去抖落盘
│   │   └── …（其余编排源文件）
│   ├── render/               # 渲染层
│   │   ├── RenderManager.h / .cpp# 窗化 / GDI 两态渲染
│   │   ├── DirectCompRenderer.*  # DirectComposition 合成
│   │   ├── D2DRenderer.*        # Direct2D 绘制
│   │   └── …（其余渲染源文件）
│   ├── platform/             # Win32 封装层
│   │   ├── WindowManager.*   # 窗口 / 毛玻璃 / 圆角 / DPI
│   │   ├── AutoStart.*       # 开机自启动（计划任务 + 注册表）
│   │   ├── TrayIcon.*        # 系统托盘
│   │   ├── IconProvider.*    # 图标提取
│   │   ├── MouseHook.* / EventHook.*
│   │   └── …（其余平台封装）
│   └── utils/                # 工具
│       └── …
├── res/
│   ├── config.json          # 默认配置（见 §8）
│   └── icons/               # 图标资源（tray_icon.png 母版 + 嵌入用 tray_icon.ico）
├── tools/
│   ├── gen_app_icon.py      # 从 tray_icon.png 生成多尺寸 tray_icon.ico（256–16）
│   ├── msvc_env.py          # 构建期 MSVC/SDK 环境推导（build.bat 用）
│   └── pe_icon_res.py       # 校验 exe 是否内嵌图标（构建后核验，退出码 0/1）
├── docs/                     # 文档
├── build/                    # 构建产物（git-ignored）
└── release/                  # 分发目录（build.bat 打包生成）
```

> `docs/`、`build/`、`release/` 均为非源码目录；`build*` 由 `.gitignore` 忽略。**构建入口说明**：`build.sh` 是主推的跨 Shell（Git Bash / MSYS2 / WSL）构建脚本，仅依赖 VS + Ninja；`build.bat` 是**可选的、遗留保留**的 Windows 原生（cmd）构建入口，标志面与产物与 `build.sh` 一致（`build/`），但额外依赖 **Python 3 + `tools/msvc_env.py`** 推导工具链，且 RELEASE 时会把 `release/` 打包为「可直接分发」目录（`openDock.exe` + `config.json` + `res\` + MSVC CRT/UCRT dlls）。**新用户与 CI 一律优先使用 `build.sh`**；`release/` 由 `build.bat` 的打包步骤生成，不在 `build.sh` 主构建链路内。

---

## 配置说明

### 配置文件位置

- **默认 / 源码侧**：`res/config.json`。
- **运行侧**：CMake 在 configure 阶段将其复制到 `build/config.json` 与 `build/res/config.json`（兼容双击 exe 时两种候选解析路径），以 **先命中者** 生效。

配置为手写解析的 JSON（无第三方依赖），落盘采用「写临时文件 + 原子替换」，并去抖 800ms 合并多次改动，避免写入中断导致的损坏。

### 字段一览

| 字段路径 | 类型 | 默认 | 说明 |
|----------|------|------|------|
| `dock.position` | string | `"top"` | 主 Dock 停靠边：`top` / `bottom` / `left` / `right`（四边可同时启用，见 `edgeEnabled`） |
| `dock.iconSize` | int | `64` | 图标基准边长（px） |
| `dock.maxScale` | float | `2.5` | 鱼眼最大放大倍率 |
| `dock.iconSpacing` | int | `10` | 图标间距（px） |
| `dock.magnifyRadius` | int | `4` | 受放大影响的邻域图标数量（单侧） |
| `dock.bounceAmplitude` | int | `30` | 弹跳动画幅度（px） |
| `animation.stiffness` / `animation.damping` | float | — | 弹簧系统的刚度 / 阻尼，决定动画手感 |
| `appearance.backgroundOpacity` | float | `0.4` | Dock 背景不透明度（0~1） |
| `appearance.blur` | bool | `false` | 是否启用背景毛玻璃 |
| `appearance.cornerRadius` | int | `20` | 圆角半径（px，Win11 生效） |
| `appearance.shadowEnabled` | bool | `true` | 是否绘制阴影 |
| `appearance.tooltipEnabled` | bool | `true` | 悬停是否显示名称 Tooltip |
| `appearance.dockBarVisible` | bool | `false` | 是否显示 Dock 底座条 |
| `display.monitor` | int | `0` | 目标显示器索引（0 = 主显示器） |
| `autoHide.autoHide` | bool | `true` | 是否边缘自动隐藏 |
| `autoHide.showDelay` / `autoHide.hideDelay` | int | `0` / `0` | 显示 / 隐藏延迟（ms） |
| `autoStart` | bool | `false` | 是否开机自启动（计划任务优先；标准用户创建任务失败时回退 HKCU\Run，存在启动错峰，见已知限制 §8） |
| `edgeOffset` / `centerOffset` | int | `0` / `0` | 距边 / 距中心偏移（px） |
| `zOrder` | string | `"bottom"` | 层级：`"top"`(1) / `"normal"`(0) / `"bottom"`(-1) |
| `edgeEnabled` | bool[4] | `[true,true,true,true]` | 四边启用开关，顺序 `[下,上,左,右]` |
| `icons[]` | array | 记事本/cmd/资源管理器/计算器 | 主图标集，每项 `{path,index,name,args,workingDir}` |
| `edges.top/bottom/left/right[]` | array | — | 各边的独立图标集，覆盖默认 `icons`；留空则该边沿用 `icons` |

> **四边图标集**：`edges` 对象允许为每条边单独配置图标列表（如 `edges.top` 与 `edges.bottom` 不同）。`ConfigManager` 已实现按边解析（含针对单键解析的 D8 修复），但任意边未显式配置时回退使用 `icons` 主集。

### 修改生效

- 运行时通过界面交互（拖入/拖出、托盘菜单）产生的改动会**自动去抖落盘**到当前生效的 config 路径。
- 直接手改 `res/config.json` 后，需重新构建或确保 `build/` 下的副本同步（`copy_dock_icons` 仅同步 `icons/` 资源，配置副本以 configure 阶段复制为准；改配置后建议 `--clean` 重建或手动同步）。

---

## 已知限制 / 未实现项

以下为当前工程**已知且刻意或暂未覆盖**的范围，非缺陷列表之外需关注的点：

1. **仅支持 x64**：工具链、`INCLUDE` / `LIB` 均硬编码 x64，未提供 x86 / ARM64 构建配置（见 §2 系统要求）。
2. **无内置图标库 / Docklet 插件体系**：图标来自拖入或 `config.json` 显式配置，工程未实现「内置图标库」「Docklet 插件」等可扩展位。
3. **未做国际化（i18n）**：托盘菜单与提示均为中文，未抽象多语言资源；非中文环境 UI 文本不变。
4. **Dock 右键上下文菜单已移除**：在 Dock 条上右键不再弹出配置菜单（已被刻意移除）；删除图标通过「拖动图标离开本边感应区」完成。退出等全局操作统一走**系统托盘右键菜单**。这是设计意图，不是遗漏。
5. **无 ctest 单元测试，无自动化验证套件**：`CMakeLists.txt` 已移除 `test_*` 目标与 `--verify` / `--acceptance` 无头套件，回归纯应用构建；质量需在本机 GUI 中人工回归（见「质量与验证说明」）。
6. **GDI 回退路径功能降级**：`--force-gdi` 或 DirectComposition 不可用时走 GDI + Layered Window，该路径下毛玻璃与 DWM 圆角不可用，仅保证基础显示与交互。
7. **`release/` 分发目录由 `build.bat` 打包生成，不在主构建链路内**：`release/` 不是 `build.sh` 的产物；仅当用 `build.bat` 做 RELEASE 构建时，才会把 `openDock.exe` + `config.json` + `res\` + MSVC CRT/UCRT dlls 打包进 `release/`。升级请用 `build.sh`（或 `build.bat`）从源码重建，不要直接依赖历史 `release/` 二进制。
8. **标准用户的自启限制**：非管理员账户创建计划任务会被系统拒绝访问，此时 `AutoStart` 回退到 `HKCU\...\Run`；Windows 对 Run 键启动项有约 1 分钟错峰延迟（`StartupDelayInMSec` 在 Win11 上实测无效）。若需开机秒级拉起，请以管理员身份运行一次 openDock 并勾选「开机自动启动」，让计划任务创建成功（任务建成后，之后标准用户登录仍由 Task Scheduler 秒级触发）。

---

## 许可证与贡献

- **许可证**：本仓库当前未随附 `LICENSE` 文件，授权条款尚未确定。在条款明确前，请暂勿基于源码进行再分发或闭源衍生。
- **贡献约定**：
  - 新功能 / 修复需在本机 GUI 中人工回归：启动后逐边唤出、拖入/拖出、托盘菜单、自启动等核心路径无回归。
  - 配置、渲染、平台封装的改动请在 PR 描述中附上本机回归步骤与观察结果。
  - 保持零第三方依赖原则：新增能力优先用 Windows SDK 系统库或手写实现。
