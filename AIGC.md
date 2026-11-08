# U 盘助手 · 项目笔记（交接给 AI 继续开发用）

> 项目：`C:\Users\24306\Documents\Trae-CN\temp_20261107_02`
> 产物：`build/bin/UsbAssistant.exe`
> 技术定位：C++17 + 纯 Win32 API + Direct2D/DirectWrite 自绘，无任何第三方 UI 库。

---

## 1. 环境与构建

- 编译器：MinGW-w64 g++（`E:\mingw64\bin`），CMake ≥ 3.20，生成器 `MinGW Makefiles`；也保留 MSVC 分支。
- 关键定义：`UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00`；MSVC 额外 `/utf-8`。
- 链接库：`d2d1 dwrite dwmapi shell32 shlwapi setupapi cfgmgr32 ole32 user32 gdi32 advapi32 uuid`，MinGW 再加 `gdiplus version` 与静态运行时 `-static -static-libgcc -static-libstdc++ -lwinpthread`。
- 构建命令：

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

- 输出目录固定在 `build/bin`（`RUNTIME_OUTPUT_DIRECTORY`），Release 默认。
- 改资源后若图标没变：CMake **不跟踪 `.ico` 依赖**，需先 touch `res/app.rc`（更新其修改时间）再 `cmake --build build`。

---

## 2. 目录与模块

```
CMakeLists.txt
docs/screenshots/          README 图片（main/settings/empty/tray-menu/edge-bar）
res/app.rc                 资源脚本（1 ICON "app.ico"）
res/app.ico                自绘原创图标，10 个尺寸 16~256
res/icon_preview.png       README 头部图标
src/main.cpp               入口、单实例、--autostart
src/app.h/.cpp             窗口、消息、拖拽、动画、托盘、边缘悬浮条、弹出结果复核
src/canvas.cpp/.h          DIB + D2D DC 渲染目标 + UpdateLayeredWindow 呈现
src/render.cpp             全部界面绘制（卡片/按钮/弹层/滚动条/空状态/toast）
src/device.cpp/.h          SetupAPI 枚举可移动设备、按物理磁盘分组
src/eject.cpp/.h           安全弹出（PnP 链式移除 / 介质弹出 / 结果校验）
src/occupant.cpp/.h        NtQuerySystemInformation 句柄扫描，定位/结束占用进程
src/settings.cpp/.h        config.ini 读写、开机自启快捷方式
src/settings_window.cpp    设置窗口（9 行、下拉框、开关、百分比填空）
src/tray_menu.cpp/.h       自绘托盘右键菜单（模态循环）
src/theme.cpp/.h           明暗配色 Palette、QuerySystemDarkMode
src/com_ptr.h              COM 智能指针
```

---

## 3. 关键技术方案

### 3.1 分层窗口 + D2D
- 主窗 `UsbAssistantWindow`：`WS_POPUP` + `WS_EX_LAYERED | WS_EX_TOOLWINDOW`（置顶时加 `WS_EX_TOPMOST`）。
- 窗口四周预留 `ui::kShadowPad = 15.0f` 逻辑像素透明阴影；面板宽 `ui::kPanelW = 372.0f`，因此窗口逻辑尺寸为 `402 × (panelH + 30)`。
- 绘制流程：`Canvas::Begin(dpi)` → D2D 绘制 → `Canvas::End()` → `Canvas::Present(hwnd, alpha)`（内部 `UpdateLayeredWindow` 逐像素 alpha 合成）。
- `WM_NCHITTEST`：只有落在圆角面板（`InRoundedRect(layout_.panel, 16, ...)`）内才返回 `HTCLIENT`，否则返回 `HTTRANSPARENT`，阴影区不拦截鼠标。
- 动画（淡入淡出、滚动、hover、悬浮条滑入）统一由一个 `kTimerAnim` 驱动的 `AnimTick()` 推进，改值后 `StartAnim()`。

### 3.2 拖拽与边缘悬浮条
- 自绘窗口不走系统移动循环：`WM_LBUTTONDOWN` 在标题栏区域 `SetCapture`，`WM_MOUSEMOVE` 用 `GetCursorPos` + 初始窗口矩形算偏移，`WM_LBUTTONUP` 释放并 `CheckDropPosition()`；`WM_EXITSIZEMOVE` 也会再判一次。
- 边缘判定（要点：**用内缩阴影后的真实面板矩形**；**任务栏所在边不参与**）：

```cpp
int pad = lround(ui::kShadowPad * scale_);
RECT panel = {win.left + pad, win.top + pad, win.right - pad, win.bottom - pad};
SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
bool allowLeft  = work.left   <= 0;
bool allowTop   = work.top    <= 0;
bool allowRight = work.right  >= GetSystemMetrics(SM_CXSCREEN);
bool allowBottom= work.bottom >= GetSystemMetrics(SM_CYSCREEN);
int margin = lround(24.0f * scale_);          // 距边缘 24 逻辑像素内才触发
int farScore = 100000;                        // 变量名不能叫 far（windef.h 宏）
int leftScore  = allowLeft  ? panel.left  - (work.left  + margin) : farScore;
int rightScore = allowRight ? (work.right - margin) - panel.right : farScore;
int topScore   = allowTop   ? panel.top   - (work.top   + margin) : farScore;
int bottomScore= allowBottom? (work.bottom- margin) - panel.bottom: farScore;
int best = min(min(leftScore,rightScore), min(topScore,bottomScore));
if (best > 0) return;                         // 没贴边、也没出界
int edge = best == leftScore ? 0 : best == rightScore ? 1
         : best == topScore  ? 2 : 3;         // 0左 1右 2上 3下
CollapseToEdgeBar(edge);
```

- 悬浮条 `UsbAssistantEdgeBar`：`WS_POPUP` + `WS_EX_TOPMOST | WS_EX_TOOLWINDOW`，`side = lround(56 * 0.75) = 42px`（上限按工作区、下限 32），`CreateRoundRectRgn(0,0,side+1,side+1,14,14)`，贴住触发的边缘：左右用 `panel.top`、上下用 `panel.left` 对齐。
- 悬浮条窗口只用 GDI `DrawIconEx` 把大图标铺满，背景按主题白/深灰。
- 点击恢复 `RestoreFromEdgeBar()`：先算可见矩形 `restoreX_/restoreY_`，再按 `edgeSide_` 设置滑入偏移（左 -72 / 右 +72 / 上 -72 / 下 +72 像素），`ShowWindow(SW_SHOWNOACTIVATE)` 后由动画把偏移归零。
- `settings_.edgeCollapse == false` 时，`CheckDropPosition()` 与 `CollapseToEdgeBar()` 都直接 return，绝不创建悬浮条。
- 悬浮条与主窗都用 `WS_EX_TOOLWINDOW`，不占任务栏按钮；`WM_MOVE` 时 `RecordWindowPosition()`。

### 3.3 设备枚举（device.cpp）
- `SetupAPI` 遍历 `GUID_DEVINTERFACE_DISK`（手写 GUID 避免 uuid 依赖），读 `STORAGE_DEVICE_DESCRIPTOR` 取型号，仅保留 USB 总线/可移动介质，按磁盘号分组为 `DeviceInfo`。
- 标题清洗：去掉 `USB / Generic / Mass / Storage / 2.0 / 3.x` 等通用词，剩余部分做卡片名。
- 分区信息：`GetVolumeInformation`（卷标、文件系统）+ `GetDiskFreeSpaceEx`（总量/剩余）；只有资源管理器能打开的文件系统才置 `openable`。
- 卷 GUID 路径的坑：`CreateFile` 不接受末尾反斜杠，而 `GetVolumeInformation / GetDiskFreeSpaceEx` 要求带反斜杠 → 分开处理（`OpenableVolumePath()`）。

### 3.4 安全弹出（eject.cpp）
顺序（`EjectDisk(diskNumber, title, volumes)`）：
1. 逐卷 `CreateFile` + `FSCTL_LOCK_VOLUME`（3 次 × 60ms 重试）并卸载，失败记录错误。
2. `TryPnpEject`：从磁盘节点向上收集到 USB 节点，**从最上层（USB\VID_...）开始** `CM_Request_Device_EjectW`。直接弹磁盘节点常被存储驱动否决，必须请求父设备。
3. 首次 PnP 失败后释放卷句柄再试一次（自己的打开句柄会造成 `PNP_VetoOutstandingOpen`）。
4. **快速占用返回**（关键性能修复）：若重试被明确否决为 `PNP_VetoOutstandingOpen` 或 `PNP_VetoWindowsApp`，立即返回 `Busy`，不再走耗时的介质弹出与等待，占用弹窗立刻出现。
5. 回退 `IOCTL_STORAGE_EJECT_MEDIA`（读卡器/部分 U 盘）。
6. 成功判定 `WaitDiskGone()`：磁盘设备节点消失 **或** 该设备所有盘符都消失（`GetLogicalDrives()` 位掩码，`AnyVolumeLetterPresent`）即为成功；超时 4000 / 4000 / 3500ms。
7. 错误码映射：`ERROR_ACCESS_DENIED → AccessDenied`、`ERROR_INVALID_FUNCTION / ERROR_NOT_SUPPORTED → Unsupported`、`ERROR_NOT_READY / ERROR_NO_MEDIA_IN_DRIVE → NotFound`，其余 `Failed` 附错误码；否决原因用 `VetoMessage(vetoType, vetoName)` 生成中文说明。

弹出结果复核（app.cpp，修复「已弹出却提示失败」）：
- `WM_APP_EJECT_DONE` 里 `ok == false` 时**不立刻报错**，只记录 `ejectVerifyPending_ / ejectVerifyDisk_ / ejectVerifyTitle_ / ejectVerifyMessage_`。
- 等下一次设备枚举结果 `OnDevicesReady()`：若还有排队刷新（`refreshQueued_`），说明这份列表可能早于弹出完成，丢弃并再刷一次；否则查看 `devices_` 里是否还有该 `diskNumber`：没有 → 成功 toast，有 → 失败 toast。
- 成功路径：`ShowToast` + `SetTimer(kTimerRefresh, 500)` 延迟刷新。

### 3.5 占用检测与解除（occupant.cpp）
- `NtQuerySystemInformation(SystemExtendedHandleInformation = 64)` 全量枚举句柄 → 对候选句柄 `DuplicateHandle` 后 `NtQueryObject` 取对象类型/名称（`kObjectNameInformation=1`、`kObjectTypeInformation=2`）。
- 按 `objectTypeIndex` **缓存类型判断**，避免逐句柄查类型造成卡顿；只保留打开目标卷设备路径（`\Device\HarddiskVolumeN` 映射）的进程，按 PID 去重。
- 显示名取文件版本 `FileDescription`，缺失退回文件名；图标 `SHGetFileInfo / ExtractIcon`，用 `FreeOccupants()` 释放。
- 解除占用：`OpenProcess` + `TerminateProcess`，跳过自身 PID 与系统关键进程，**不结束 explorer.exe**。
- 扫描在后台线程跑，结果经 `WM_APP_OCCUPANTS_READY` 回主线程补入弹窗列表（弹窗先显示、扫描完成再填充）。

### 3.6 托盘、设置窗口、托盘菜单
- 托盘：`Shell_NotifyIconW`，回调消息 `WM_APP_TRAY (WM_APP+2)`；左键 `ShowFromTray()`，右键 `ShowTrayMenu()`；监听 `TaskbarCreated` 重新添加图标。
- 托盘菜单 `ShowTrayMenuPopup`：172 逻辑宽，`kItemH=34`、`kPad=6`、危险项前 `kGap=6`（3 项高 120）；弹出位置 = 光标 `x-w+12*scale`、`y-h-10*scale`，再夹到工作区。
  - **必须 `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE` + `SW_SHOWNOACTIVATE`**：一旦激活，Windows 托盘折叠飞出区会被打断而自动消失。
  - 因为不激活、收不到键盘，用 `MsgWaitForMultipleObjectsEx(40ms)` 轮询：点击菜单外或按 Esc 就关闭；返回被点项索引给 `ShowTrayMenu()` 分发（0 显示窗口 / 1 设置 / 2 退出）。
- 设置窗口 `UsbAssistantSettings`：非模态单例（`g_wnd` 存在就 `SetForegroundWindow`），`WS_POPUP` + 16px 圆角 region，400×546 逻辑、按 DPI 缩放并居中工作区；标题「U 盘助手 - 设置」。
- 设置项 9 行：开机自启动 / 始终置顶 / 默认位置 / 容量进制 / 关闭按钮功能 / 未插入时自动隐藏 / 靠边隐藏到悬浮窗 / 主界面最大高度 / 刷新间隔；下拉项用独立弹层窗口（`settings_window.cpp` 内 `PaintList`），百分比为就地填空输入。
- 设置变更统一 `SaveSettings()` 后回调 `App::ApplySettings()`（重设进制、置顶、刷新计时器、自动隐藏计时）。

### 3.7 主题 / DPI / 字体
- `MakePalette(QuerySystemDarkMode())`：`WM_SETTINGCHANGE / WM_THEMECHANGED` 时 `UpdateTheme()` 重取配色并重绘。
- `WM_DPICHANGED`：更新 `dpi_/scale_`、重算面板高度与布局、`ApplyWindowSize()`、按建议矩形移动。
- 字体统一 `Microsoft YaHei UI`，失败回退 `Segoe UI`，`zh-cn`；文字抗锯齿 `GRAYSCALE`。
- 面板高度：`min(内容需要高度, 屏幕可用高度 × MainHeight% − 2×阴影)`，下限 120；超出改滚动（`scrollMax`、`scrollY_`、悬浮加粗滚动条）。

### 3.8 线程与消息
- 后台线程：设备枚举（`RefreshState` + mutex + `ready` 标志）、弹出、占用扫描。结果通过 `PostMessage` 回主线程：

| 消息 | 值 | 用途 |
| --- | --- | --- |
| `WM_APP_EJECT_DONE` | `WM_APP+1` | 弹出线程完成 |
| `WM_APP_TRAY` | `WM_APP+2` | 托盘图标回调 |
| `WM_APP_SHELL_NOTIFY` | `WM_APP+3` | 卷可用空间/介质增删通知，合并为 280ms 后刷新 |
| `WM_APP_DEVICES_READY` | `WM_APP+4` | 设备枚举结果就绪 |
| `WM_APP_SHOW_MAIN` | `WM_APP+5` | 单实例激活已运行窗口 |
| `WM_APP_OCCUPANTS_READY` | `WM_APP+6` | 占用扫描结果就绪 |

- 定时器（`ui` 命名空间）：`kTimerAnim=1`、`kTimerRefresh=2`、`kTimerToast=3`、`kTimerPoll=4`、`kTimerEmptyHide=5`、`kTimerBusyScan=6`。

---

## 4. 踩坑清单（按坑的严重程度）

1. **windres 不支持 PNG 压缩的 ICO**：报 `unexpected EOF`。必须写 32 位 DIB 条目：`BITMAPINFOHEADER`（40 字节，`biHeight = 2h`、`biBitCount=32`、`BI_RGB`）+ **自下而上 BGRA** + 全零 AND 掩码（行按 `((w+31)/32)*4` 对齐）。
2. **CMake 不跟踪 `.ico`**：改图标后必须 touch `res/app.rc`（`(Get-Item res\app.rc).LastWriteTime = Get-Date`）才会重跑 windres。
3. **系统图标缓存**：exe 图标已换但资源管理器没变时，`ie4uinit.exe -show`（必要时 `-ClearIconCache`）；验证用 `ExtractIconEx` / `SHGetFileInfo` 直接读 exe 内图标。
4. **`far` 是 windef.h 的宏**：局部变量不能叫 `far`，否则编译出现莫名其妙的错误 → 用 `farScore`。
5. **边缘判定必须去阴影**：用窗口矩形会把 15px 透明阴影算进去，导致"没贴边也收起" → 内缩 `kShadowPad*scale`。
6. **任务栏边不参与**：用 `SPI_GETWORKAREA` 与 `SM_CXSCREEN/CYSCREEN` 比较判断哪条边被任务栏占用。
7. **直接弹磁盘节点会被否决**：`CM_Request_Device_EjectW` 要请求父级 USB 设备节点。
8. **自己的卷句柄导致 `PNP_VetoOutstandingOpen`**：弹出前 `closeAll()`，失败后释放句柄再重试。
9. **`WaitDiskGone` 不能只看设备节点**：部分设备节点残留但盘符已消失，应两个条件任一满足即成功。
10. **枚举结果可能比弹出完成更早**：复核时必须处理 `refreshQueued_`（旧列表丢弃重刷），否则会误报失败。
11. **占用弹窗延迟数秒**：PnP 否决为 OutstandingOpen/WindowsApp 时直接返回 Busy，跳过介质弹出与等待。
12. **`FSCTL_LOCK_VOLUME` 重试次数不要太多**：3 次 × 60ms 足够，否则整体变慢。
13. **托盘菜单不能用普通弹出窗口**：会被折叠飞出区的激活逻辑打断；必须 `NOACTIVATE` + 轮询关闭。
14. **设置窗口要非模态单例**：`g_wnd` 复用，避免重复开窗和状态不同步。
15. **GDI+ `GraphicsPath` 不能拷贝构造**（生成图标时）：改为 `RoundedRect(GraphicsPath&, ...)` 传引用。
16. **PowerShell 画渐变笔刷重载解析易失败**：用 `LinearGradientMode::ForwardDiagonal`；还不行就放弃高光层。
17. **PowerShell 手写 ICO 时括号/字节数组易解析失败**：拆成 `$h2`、`$imgSize` 等临时变量，别在一行里嵌套转换。
18. **截图 `Bitmap::new(w,h)` 宽高为 0 会抛"参数无效"**：先校验 `> 0` 且窗口在屏内再截。

---

## 5. 配置（config.ini）

位置：exe 同目录，不可写时退回 `%APPDATA%\UsbAssistant`；`[General]` 段，首次运行保存。

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `TopMost` | `1` | 始终置顶 |
| `PosMode` | `0` | `0` 固定角落，`1` 记住上次位置 |
| `Corner` | `3` | `0` 左上、`1` 左下、`2` 右上、`3` 右下 |
| `RefreshSec` | `3` | 轮询间隔（秒），`0` 关闭 |
| `CapacityBase` | `1024` | `1000` 为硬盘厂商进制 |
| `CloseAction` | `0` | `0` 退出，`1` 隐藏到托盘 |
| `SkipCloseConfirm` | `0` | `1` 关闭不再确认 |
| `EmptyHideSec` | `0` | 无设备自动隐藏秒数，`0` 不隐藏 |
| `EdgeCollapse` | `0` | `1` 允许靠边收起悬浮条 |
| `MainHeight` | `90` | 主界面最大高度百分比（20–100） |
| `HasLastPos` / `LastX` / `LastY` | `0/0/0` | 上次位置（`PosMode=1` 时用） |

> 本机开发目录当前值（非默认）：`Corner=1`、`CloseAction=1`、`SkipCloseConfirm=1`、`EdgeCollapse=1`、`HasLastPos=1`、`LastX=-101`、`LastY=440`、`MainHeight=50`。

---

## 6. 调试与自动化验证（无 UI 测试框架，全靠 Win32 消息）

- 启动：`build\bin\UsbAssistant.exe`；重复启动会激活已有窗口（单实例）。
- 找窗口：`FindWindowW("UsbAssistantWindow"/"UsbAssistantSettings"/"UsbAssistantEdgeBar"/"UsbAssistantTrayMenu", nullptr)`。
- 开托盘菜单：`SetCursorPos(1200,660)` 后 `PostMessageW(main, 0x8002 /*WM_APP_TRAY*/, 0, 0x0205 /*WM_RBUTTONUP*/)`。
- 点菜单项：向菜单窗口 `PostMessageW(menu, 0x0202 /*WM_LBUTTONUP*/, 0, MAKELPARAM(80, 57*scale))`；第 2 项「设置…」逻辑 y≈57 → 打开设置窗口。
- 触发边缘收起（模拟拖拽结束）：`SetWindowPos(main, 0, work.right-20, 300, 0,0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE)` 后 `PostMessageW(main, 0x0232 /*WM_EXITSIZEMOVE*/, 0, 0)`。
- 点悬浮条恢复：实测只发 `WM_LBUTTONUP` 有时不生效，**同时发 `WM_LBUTTONDOWN` + `WM_LBUTTONUP`** 才稳定。
- 截图：`System.Drawing.Graphics.CopyFromScreen`；主界面要裁掉阴影：`pad = round(15 * scale)`，`scale = 窗口宽 / 402`。
- 截图时若设备卡片被高度上限截断，临时把 `MainHeight` 改 90，采完把 `config.ini` **原文内存保存后写回**（不要依赖备份文件，曾出现备份"找不到路径"导致配置残留）。
- 当前运行环境：1366×728 工作区、100% 缩放、浅色主题；无 U 盘时为空状态。

---

## 7. 约束与注意事项

- 只用 Win32 + D2D/DWrite，**不引入 MFC/Qt/第三方 UI 库**；C++17、全 Unicode（`W` 版 API）。
- 界面文案中文，不使用 emoji；README 可适量用 emoji。
- 托盘菜单项固定为「显示窗口 / 设置… / 退出」，退出项红色（`dangerIndex=2`）。
- 占用进程的结束逻辑**必须排除 explorer.exe 与本进程**，避免误杀桌面。
- 悬浮条创建前先检查 `settings_.edgeCollapse`；任务栏边永不触发。
- 所有窗口自绘 + 分层，坐标一律用 **逻辑像素 / `scale_`** 换算，避免高 DPI 下错位。
- `res/app.ico` 为项目自绘原创图标（圆角蓝紫渐变 + 白色"弹出"符号），无第三方素材，可自由商用；README 已声明。
- `res/RCa06988` 是 windres 预处理残留的临时文件（UTF-16 展开的 `.rc`，272 字节），不参与构建，可直接删除；建议 `.gitignore` 加 `res/RCa*`。
- `.gitignore` 已存在：忽略 `build/`、`build-msvc/`、`cmake-build-*/`、CMake 生成文件、`config.ini`、`.vscode/`、`.idea/`、`*.user`、`Thumbs.db` 等。
- 沙箱环境下跑 GUI/窗口消息/截图脚本时，Shell 命令需 `dangerouslyDisableSandbox: true`。
- PowerShell 调 `FindWindowW` 传空窗口名要用 `[NullString]::Value`；`Add-Type` 里的 `bool` 参数不能传字符串 `"true"`。

---

## 8. 当前状态（已验收）

- 功能全部完成并实测：设备列表、安全弹出、占用检测/解除、边缘悬浮条（左/右/上，下边缘因任务栏不触发）、托盘菜单、设置窗口、明暗主题、DPI 自适应、单实例与 `--autostart`。
- 实测记录（工作区 0,0–1366,728）：右侧条 `1324,415-1366,457`（恢复后主窗 `964,400-1366,680`）；左侧条 `0,415-42,457`；上侧条 `515,0-557,42`；底部拖出无悬浮条。
- 已清理死代码：未使用常量 `kWindowWLogical`、三个死声明（`CreateMainWindow` / `CreateDeviceObjects` / `ApplyDpi`）。
- README 已完成：徽章、5 张预览图（`docs/screenshots/` 下 `main.png` 372×418 带双 U 盘、`settings.png` 400×546、`empty.png` 372×250、`tray-menu.png` 172×120、`edge-bar.png` 97×134）、技术栈、目录结构、构建、使用、配置表、实现要点、图标许可、MIT。
- 仓库**尚未 `git init`**；上传 GitHub 前的建议：

```powershell
git init
git add .
git commit -m "U 盘助手: Win32/D2D 悬浮窗工具"
git branch -M main
git remote add origin <repo-url>
git push -u origin main
```

- 可选后续：补深色主题/占用弹窗截图、GitHub Actions 用 MinGW 交叉构建并上传 Release、给 README 加运行效果 GIF。
