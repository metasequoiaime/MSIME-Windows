# AGENTS.md — server

组织级约定见 [组织 AGENTS.md](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md)，仓库地图和组件之间的边界见[仓库根 AGENTS.md](../AGENTS.md)。本文件补充本组件的实现、数据和验证规则，路径都相对于 `server/`。

本组件是常驻后端进程：输入引擎与候选状态、配置、词典、Named Pipe 服务，以及候选窗、悬浮工具栏、托盘菜单、设置窗口的原生宿主与 WebView2 控制器。

## vcpkg_installed 的解析

`CMakeLists.txt` 四处引用 `vcpkg_installed`，全部走 `${CMAKE_BINARY_DIR}`。其中 `WEBVIEW2_VCPKG_ROOT` 供 WebView2 loader 的导入库和运行时 DLL 拷贝使用。

**不要改回 `${CMAKE_SOURCE_DIR}/build/...`。** 那样 binary dir 就只能叫字面的 `build`，本仓自带的 `vcpkg-release` preset（`binaryDir` 是 `build-release`）和 `scripts/lcompile-release.ps1` 都会在链接 `MetasequoiaImeSettings` 时报 LNK1181 找不到 `WebView2Loader.dll.lib`。

CI 刻意构建到 `build-release` 而不是 `build`，为的就是让这条不会悄悄退回去——两者在目录名恰好是 `build` 时解析结果相同，所以只有构建到别处才测得出来。

## 测试跑在真实词库上，本地要先备好数据

`tests/CMakeLists.txt` 构建 `MetasequoiaImeServerTests`，`tests/src/` 下有 34 个测试源文件。根 `CMakeLists.txt:8` 的 `include(CTest)` 让 `BUILD_TESTING` 默认为 ON，`:420` 据此 `add_subdirectory(tests)`，而 `tests/CMakeLists.txt:78` 用 `add_test` 注册了一条同名用例。

CI 现在会真的跑它：根 `.github/workflows/ci.yml` 的 Server job 在 Build 之后执行 `scripts/ci/test-server.ps1`，那个脚本按 `product-lock.json` 取词库、备好数据根目录，再跑 `ctest --test-dir server/build-release -C Release --timeout 120`。断言失败会让 CI 变红，不再是编译通过就算数。

**关键在于这些测试依赖真实词库，不是 fixture。**引擎从数据根目录读数据，默认 `%LOCALAPPDATA%\metasequoiaime`，可用 `METASEQUOIA_IME_DATA_DIR` 环境变量覆盖。`scripts/ci/test-server.ps1` 往那里放了四样东西，本地跑测试要凑齐同样的：

- `msime.db`、`others.db`、`english.db`——从产品锁指定的 `dict-*` release 下载，CI 逐个核对 `SHA256SUMS.txt`。词库损坏要立刻失败，而不是拖到测试里表现成「候选为空」这种难查的样子
- `helpcodes/`——五套辅助码方案，本仓 `assets/tables` 只有其中一套，得从 `../engine/helpcode/` 取全
- `assets/tables/*` 和 `assets/config/config.toml`

数据不全时的典型症状是候选查询返回空集，断言信息看起来像逻辑错误，实际是缺数据。排查测试失败前先确认数据根目录是齐的。

IPC 线格式、opcode 和语音分帧的唯一实现位于 `../engine/contracts/`——引擎已内嵌进本仓，和 Server 在同一个提交里；本仓的 `src/ipc/ipc.h` 只保留连接状态和 Server API。不要重新复制协议定义。共享头的 ABI 断言随主工程编译，主连接必须通过 `NegotiateMainPipeClient` 后才能激活；旧 DLL 的未版本化 hello 仍兼容，版本化客户端会收到关联 request_id 的协议确认。

改了协议或候选逻辑，正常构建并备好数据之后自己跑一次：

```powershell
# scripts/lcompile-release.ps1 默认只构建 MetasequoiaImeServer 目标（打包用的那一套），
# 不含测试可执行文件。要跑 ctest 就得显式构建全部：
.\scripts\lcompile-release.ps1 -Target ALL_BUILD
ctest --test-dir build-release -C Release --output-on-failure
```

## uiAccess 与 Topmost 时序

`MetasequoiaImeServer.manifest` 用 `requestedExecutionLevel level="asInvoker" uiAccess="true"`，浮层才能盖在高完整性宿主之上。`uiAccess` 真正生效还依赖正确签名和安装在 Windows 认可的位置，清单、签名、安装路径是同一套发布约定。

进程在 `src/main.cpp:90` 设 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`。

**创建 HWND 时不能直接带 `WS_EX_TOPMOST`。** 在 `uiAccess=true` 进程里，Topmost 父窗口会让 WebView2 的跨进程 `SetParent` 返回 `E_INVALIDARG`，或者形成「窗口存在、尺寸正确但永远空白」。现有顺序是：非 Topmost 建窗 → DWM cloak 下预热 → 三个共享 environment 的 controller 依次创建并完成首屏导航 → lazy-topmost gate 打开 → 分阶段提升到 `HWND_TOPMOST`。提升后要用 `RenotifyControllerAfterPin` 重新同步 bounds 和 parent position。不要在布局函数里随手传 `HWND_TOPMOST`，也不要绕过 `EnsureSmallWindowsTopmost` / lazy pin。

隐藏尚未完成首帧的 WebView2 host 会阻断 raster 初始化，预热阶段用 cloak，之后才按配置显示或隐藏。

## Boost

`CMakeLists.txt:13` 的 `Boost_ROOT` 是有守卫的：设了 `BOOST_ROOT` 环境变量就用环境变量，否则回退到作者本机路径。**这是有意的，不要改成只认环境变量**，那会破坏作者的本地环境。CI 就是靠设这个环境变量工作的。

Boost 是静态链接但没有写进 `vcpkg.json`，所以只能用 classic 模式装，且 triplet 必须是 `x64-windows-static-md` 而不是 `x64-windows-static`——`Boost_USE_STATIC_LIBS ON` 要静态 Boost 库，而项目其余部分用动态 CRT，纯 static triplet 会让 Boost 也切到静态 CRT，链接时报 LNK2038。

## 生成文件

`scripts/prepare_env.py` 只生成本目录的 `.clangd`（从 `scripts/config_files/.clangd` 模板展开）。测试使用 Server 主工程及同一套 preset，不再生成独立的 `tests/CMakePresets.json`。`CMakePresets.json` 是**原地**改写 `VCPKG_ROOT` 与 `CMAKE_TOOLCHAIN_FILE`，不再从模板覆盖。

`CMakeLists.txt` 和 `tests/CMakeLists.txt` 不再由脚本生成，直接改就行。这两个文件曾经也是从 `scripts/config_files/` 覆盖过来的，但模板早已落后到 147 行对 400 行、以及一个无关的通用 `WinCppTemplate`，跑一次脚本就会毁掉构建，因此模板已删除。

`.clangd` 模板用 `@NAME@` 占位符，不再按固定行号替换——旧的行号索引已经漂移过一次，把 Boost 路径写到了 WebView2 的 include 上。新增 include 时在模板里加占位符、并在脚本的 `clangd_substitutions` 里补上对应项即可；有占位符没被替换脚本会直接报错。

## 提交

提交信息用 `type(scope): 摘要`。不要添加 `Co-Authored-By`、`Generated with` 或其他 AI 生成标记。

## 窗口与页面的维护边界

- **Server 拥有窗口，UiHtml 拥有页面**：候选窗、悬浮工具栏和托盘菜单的 HWND、尺寸、位置、显示、
  DPI、Z-order、WebView2 environment/controller 生命周期在 `server/src/window/` 与
  `server/src/webview2/`；页面结构、样式和浏览器端交互在 `ui-html/webview2/`。
- 消息定义以 Engine `contracts/webview/` 为准，双方必须校验。C++ → 页面更新主要经 WebView2 导航/脚本执行，页面 → C++ 动作经
  `window.chrome.webview.postMessage(...)`。修改消息 `type`、JSON 字段、页面导出的 JS 函数或 DOM
  标识时，必须同步检查 Server 的消息解析/脚本拼接与 UiHtml 的发送/接收代码。
- 候选内容和输入状态的权威仍在 Server/引擎，网页只负责展示与发出用户动作；不要在网页侧复制
  候选选择、翻页、输入模式或配置持久化的核心状态机。
- 设置页前端位于 `ui-html/webview2/settings/ime-settings/`，其中包含 Vite/TypeScript
  工程；设置窗口的原生创建、激活、WebView2 承载及与配置系统的桥接仍由 Server 负责。

### 尺寸、坐标与 DPI

- **所有涉及尺寸或坐标的代码都必须明确考虑 DPI**。新增或修改窗口大小、位置、边距、间距、字体、
  图标、命中区域、WebView2 bounds、候选布局及屏幕边界计算时，不得默认固定像素在所有显示器上等价。
- 明确每个数值使用逻辑单位还是物理像素，并在正确的窗口/显示器 DPI 下统一换算；不要混用来自不同
  DPI 空间的坐标和尺寸，也不要长期缓存可能在窗口跨屏或 DPI 变化后失效的换算结果。
- 原生窗口需处理运行时 DPI 变化（包括跨显示器移动），同步更新窗口、子窗口、WebView2 controller
  和相关布局；优先沿用项目已有的 DPI 获取与缩放辅助函数，避免各处自行实现不一致的换算规则。
- 涉及尺寸或布局的改动，至少检查 100%、125%、150%、200% 缩放，以及不同 DPI 显示器之间移动时
  是否出现裁切、错位、模糊、点击区域偏移或窗口越出工作区。
