# 架构说明

这份文档说明新版 UsbAssistant 的模块边界、线程模型和关键状态机。目标是让每一处逻辑只存在于一个地方，避免再出现旧版那种“哪里出问题就在哪里加一个标志”的补丁式结构。

## 1. 依赖方向

```text
main.cpp
   └── app/application ──────────────┐
          ├── app/worker             │
          ├── ui/*                   │
          └── core/*                 │
                                     │
ui/*  ──> core/models.h（只读数据）  │
core/* 不依赖 ui/* 或 app/* ─────────┘
```

规则：

- `core` 可以调用 Win32 / SetupAPI / CfgMgr32 / ntdll，但**不能**包含 `d2d1.h`、窗口过程或任何界面状态。
- `ui` 只负责“把数据画出来”和“把鼠标动作翻译成 `ViewAction`”，不决定业务语义。
- `app` 是唯一知道“点击弹出之后该做什么”的地方，持有窗口、托盘、后台任务和对话框状态。
- `main.cpp` 只做入口分发：普通启动、`--autostart`、`--helper`。

## 2. 模块职责

### core

| 文件 | 职责 | 不应做的事 |
| --- | --- | --- |
| `models.h/.cpp` | `Device` / `Partition` / `Occupant` / `EjectResult` 等纯数据，`Occupant` 的图标 RAII | 不持有窗口、不读配置 |
| `text.h/.cpp` | 去通用词、设备标题、容量格式化、文件版本描述 | 不访问设备 |
| `settings.h/.cpp` | `config.ini` 读写、范围归一化、启动文件夹快捷方式 | 不弹 UI |
| `storage.h/.cpp` | 卷路径、磁盘设备节点、分区表、盘符存在性、等待设备消失 | 不决定弹出策略 |
| `devices.h/.cpp` | 以卷为入口枚举 USB 磁盘、读取分区表、聚合与排序 | 不画卡片 |
| `eject.h/.cpp` | PnP → 介质 → 强制锁定/卸载三级弹出流程与结果分类 | 不弹对话框 |
| `occupants.h/.cpp` | 句柄表扫描、进程图标/名称、结束占用、关闭资源管理器窗口 | 不结束 explorer / 系统关键进程 |
| `elevation.h/.cpp` | `--helper` 参数、结果文件、UAC 启动与等待 | 不创建主窗口 |
| `update.h/.cpp` | Release 查询、版本比较、发布包匹配 | 不下载、不解压 |
| `update_schedule.h/.cpp` | 更新频率间隔与上次检查时间的读写 | 不发网络请求、不弹 UI |

`core` 的公开接口都是“输入数据 → 返回数据”，方便单元测试。`tests/core_tests.cpp` 不创建窗口即可运行。

### ui

| 文件 | 职责 |
| --- | --- |
| `theme` | 明暗两套 `Palette`，系统主题检测 |
| `canvas` | `ID2D1DCRenderTarget` + DIB + `UpdateLayeredWindow` 的离屏画布 |
| `text` | DirectWrite 字体集合、文本绘制、单行省略号、换行高度测量 |
| `draw` | 圆角矩形、阴影、USB/硬盘/SSD 矢量图标、开关、复选框、HICON 位图 |
| `main_view` | 主界面布局、绘制、命中测试、滚动/悬停动画、窗口拖拽、对话框绘制 |
| `settings_window` | 独立 HWND、自绘设置行、原生弹出菜单选项、内部滚动 |
| `tray_menu` | 自绘、`NOACTIVATE`、轮询关闭的托盘右键菜单 |
| `edge_bar` | 屏幕边缘 42px 悬浮条 |

关键约束：

- `MainView` 不拥有设备列表。它拿到的 `Model` 里是 `shared_ptr<const std::vector<Device>>`，因此设备刷新与视图重绘之间不存在悬空指针。
- `MainView` 自己保存的只有纯视图状态：滚动位置、悬停动画、按下项、窗口拖拽。业务状态（正在弹出哪块磁盘、当前弹层、占用进程列表）由 `Application` 注入。
- 鼠标交互返回 `ViewAction`（Close / ConfirmExit / Eject / Open / ReleaseOccupants / Elevate / DragEnded），不直接调用业务函数。

### app

| 文件 | 职责 |
| --- | --- |
| `worker` | 单线程串行任务队列；任务自己负责把结果 `PostMessage` 回 UI 线程 |
| `post_target` | 后台结果的安全回传目标；`WM_DESTROY` 后自动拒绝投递 |
| `application` | 窗口生命周期、托盘、设置、对话框、扫描/弹出/占用/提权编排、边缘收起、动画 |

`Application` 仍是一个较大的类，但它的成员按“窗口 / 托盘 / 设置 / 设备 / 对话框 / 任务 / 动画”分组，并且所有业务状态只有一份。旧版中同义的状态（例如 `busyDialog_`、`ejectResultPending_`、`ejectVerifyPending_`）已经被删除。

## 3. 线程模型

只有 UI 线程可以修改 `Application`、`MainView`、窗口和托盘状态。

```text
UI thread                    scan_worker              action_worker
   │                              │                        │
   ├─ RequestScan() ─────────────>│ EnumerateDevices()     │
   │                              │                        │
   │<──── kMessageScanDone ───────┘                        │
   │                                                       │
   ├─ StartEject() ───────────────────────────────────────>│ EjectDevice()
   ├─ StartOccupantScan() ────────────────────────────────>│ FindOccupants()
   ├─ ReleaseOccupantsAndRetry() ─────────────────────────>│ ReleaseOccupants() + EjectDevice()
   ├─ RunElevated() ──────────────────────────────────────>│ LaunchHelperAndWait()
   │                                                       │
   │<──────────── kMessageEjectDone / OccupantsDone / HelperDone ──┘
```

设计点：

- **两个 Worker**：扫描与动作分开。慢速枚举不会挡住用户点“弹出”，反之亦然。
- **扫描合并**：同一时刻最多一个扫描在跑；期间新的刷新请求只置 `scan_pending_`，当前结果落地后再补跑一次。避免旧版 `refreshQueued_` 与“结果可能早于弹出完成”的判定纠缠。
- **动作串行**：弹出、占用扫描、解除占用、提权助手共用 `action_worker_`，天然避免“两个弹出同时操作同一条 USB 链路”。UI 侧用 `action_busy_` 挡住重复点击。
- **generation 令牌**：每个动作开始时 `++action_generation_`，结果消息携带该值；UI 只接受与当前值一致的结果。超时看门狗会递增 generation，从而丢弃迟到的结果。
- **消息所有权**：结果用 `std::unique_ptr<Payload>` 分配，`PostMessage` 成功才 `release()`；接收端用 `unique_ptr` 接管。`PostTarget` 在 `WM_DESTROY` 时把 HWND 置空，迟到的结果不会投递到已销毁/被复用的窗口。
- **看门狗**：动作超时（扫描 15s / 弹出 30s / 解除 20s / 提权 65s）会解锁 UI 并提示，避免 `NtQueryObject` 之类极端阻塞让界面永久卡死。
- **退出不等待**：`~Application` 用 `Worker::Stop(false)` 丢弃排队任务并 detach 正在运行的任务。任务只访问 core 与 `PostTarget`，不访问 `Application`，因此进程退出不会被 60s 的 UAC 等待或设备移除等待拖住。

### 消息表

| 消息 | 值 | 载荷 / 用途 |
| --- | --- | --- |
| `kMessageTray` | `WM_APP+1` | 托盘图标回调 |
| `kMessageScanDone` | `WM_APP+2` | `ScanPayload` |
| `kMessageEjectDone` | `WM_APP+3` | `EjectPayload` |
| `kMessageOccupantsDone` | `WM_APP+4` | `OccupantsPayload` |
| `kMessageHelperDone` | `WM_APP+5` | `HelperPayload` |
| `kMessageShellNotify` | `WM_APP+6` | Shell 卷变化通知 |
| `kMessageShowMain` | `WM_APP+7` | 第二实例激活已有窗口 |
| `ui::kMessageEdgeBarClicked` | `WM_APP+40` | 悬浮条点击恢复 |

### 定时器

| ID | 用途 |
| --- | --- |
| `kTimerAnim` | 16ms 动画帧 |
| `kTimerScan` | 设备变化 300ms 防抖 |
| `kTimerPoll` | 容量轮询（`RefreshSec`） |
| `kTimerEmptyHide` | 无设备自动隐藏 |
| `kTimerTray` | 托盘图标失败重试 |
| `kTimerToast` | toast 停留 4.2s 后淡出 |
| `kTimerAction` | 动作看门狗 |

## 4. 弹出状态机

```text
                     点击「安全弹出」
                            │
                            v
                         Ejecting
                            │
          ┌─────────────────┼─────────────────┐
          │ Ok              │ Busy            │ AccessDenied
          v                 v                 v
     toast + 刷新        BusyScanning        AdminPrompt
                            │                 │
                    扫描完成 v                 │ 点击「管理员模式」
                        BusyList              v
                            │              Helping
              ┌─────────────┼───────────┐     │
              │ 解除成功     │ 解除被拒   │     ├─ Ok -> toast + 刷新
              v             v           │     ├─ Busy -> BusyScanning
          Releasing     AdminPrompt     │     └─ 其他 -> toast
              │                         │
      ┌───────┼────────┐                │
      │ Ok    │ Busy   │ 权限/关键进程   │
      v       v        v                │
   toast   BusyScanning  toast/Admin     │
```

核心原则：

1. **成功只有一个判定点**：`core::EjectDevice` 内部调用 `storage::WaitDiskGone`，磁盘节点消失或所有盘符消失即成功。
2. **占用只有一个入口**：任何阶段得到 `EjectStatus::Busy`，都进入同一个 `ShowBusyDialog` + `StartOccupantScan`，不再维护“预扫描 / 二次确认 / 迟到结果”三套逻辑。
3. **权限不足只有一个出口**：非管理员时进入 `AdminPrompt`；已经是管理员则直接 toast，避免无意义的提权循环。
4. **对话框同一时刻只有一个**：`DialogKind { None, ConfirmClose, Busy, Admin }`，不再用三个可能同时为真的布尔量。

## 5. 数据所有权

- `Application::devices_`：`shared_ptr<const vector<Device>>`。每次扫描产生一个全新不可变列表，旧列表由视图/旧消息自然释放。
- `Application::occupants_`：同样是 `shared_ptr<const vector<Occupant>>`。`Occupant` 析构时 `DestroyIcon`，因此对话框关闭或列表替换不会泄漏 HICON。
- `MainView::Model`：只保存 `shared_ptr` 与标量/字符串副本，不保存 `Application*`。
- `SettingsWindow`：持有 `Settings` 的副本，修改后通过 `ChangeHandler` 交回 `Application` 保存；它不直接写配置文件（开机自启动除外，因为那是系统快捷方式而不是配置项）。

## 6. 关键设计决定

- **单 exe 提权**：`--helper` 在单实例互斥量之前处理，所以提权实例不会撞上主实例。结果文件使用 UTF-16 + 退出码，父进程在动作 Worker 里等待。
- **移除 Shell 右键“弹出”**：旧实现的 `IContextMenu` 回退依赖 COM，并在设备忙时可能弹出系统模态框。新版三级流程已经覆盖 PnP、介质、强制卸载三种路径，去掉这一层后行为更可预测。
- **分区表对齐**：设备枚举以卷为入口拿到盘符/文件系统，再用 `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` 读取分区表，按 `StartingOffset` 一一对应；分区表里存在但 Windows 没有卷的条目会显示为 ESP / MSR / 未挂载分区。
- **任务看门狗而非 `TerminateThread`**：看门狗只解锁 UI 并让结果过期，不强制杀线程，避免在持有句柄/锁时终止线程造成更严重的问题。
- **不引入第三方 UI 库**：托盘菜单、设置窗口、悬浮条、主界面全部 Win32 + Direct2D/GDI，运行时零额外依赖。

## 7. 扩展点

- **新增设备字段**：在 `core/models.h` 加字段，在 `devices.cpp` 填充；`MainView` 只读使用。
- **新增设置项**：在 `Settings` 加字段 → `Normalize`/`Load`/`Save` → `SettingsWindow` 的 `Row` 表加一行 → 需要即时生效则在 `Application::ApplySettings` 处理。
- **新增弹层**：在 `DialogKind` 加枚举 → `MainView::BuildLayout`/`Paint*`/`HitTest` 三处补齐 → `Application` 决定何时设置。
- **新增后台动作**：在 `Application` 里定义 payload，动作 Worker 内完成并 `PostPayload`，handler 先检查 generation。

## 8. 更新流程

```text
主程序启动 ──5s──> 自动更新开启且已到频率间隔？
                        │ 否                      │ 是
                        v                         v
              updater --cleanup            updater --autocheck / --silent
                        │                         │
                        └─ 删除 .old 后退出       ├─ 检查 → 下载 → 解压
                                                  ├─ 结束主程序（若在运行）
                                                  ├─ 覆盖 UsbAssistant.exe
                                                  ├─ 本体改名 .old，写入新升级器
                                                  └─ 重新启动主程序（原先在运行时）
```

要点：

- **主程序不再处理升级器文件**：不再有 `.new` 待生效文件与延时补替换；主程序每次启动只负责“无论如何运行一次升级器”，让升级器自己清理 `.old`。
- **频率判断只在一处**：`core/update_schedule` 给出间隔与到期判断；主程序按 `UpdateFrequency` 每分钟判断一次（`软件启动时` 不轮询），升级器在每次检查后写回 `[Update] LastCheck`，所以重启不会重置计时。
- **升级器不读设置项**：自动更新开关、更新频率、静默更新只由主程序解释，升级器只认 `--cleanup` / `--autocheck` / `--silent` 三个参数；它写回的只有 `[Update] LastCheck`。这么分是因为升级器是短命进程，运行期间的定时只能由常驻的主程序负责。
- **升级器自我替换**：正在运行的 exe 不能删除但可以改名，因此先把 `UsbAssistantUpdater.exe` 改名成 `.old`，再把新版本写成正式文件名；复制失败时把 `.old` 改回来。
- **静默更新**只改变升级器的启动方式：不创建 Direct2D/窗口，发现新版本后直接走同一套安装流程（结束主程序 → 替换 → 重启）。

## 9. 已知边界

- 真实 USB 硬件上的枚举/弹出需要在目标机器上验收；无设备环境下只能覆盖空状态与安全分支。
- `NtQueryObject` 在极少数句柄类型上可能阻塞；已通过“先复制句柄 + `GetFileType` 过滤 + 15s 看门狗”降低影响，但无法保证所有驱动行为。
- 设置窗口的选项菜单使用系统原生菜单，深色模式下由系统决定配色，不跟随应用自绘配色。
