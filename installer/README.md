# Metasequoia IME Installer

本目录的脚本从 Windows 合仓目录收集产物、签名、用 Inno Setup 打成安装包。它服务两条流程：

- **正式发布**：由 `MSIME-Windows` 的 release workflow 驱动，用真实证书签名 `MetasequoiaImeServer.exe`（保证 `uiAccess` 生效）和最终安装包；产物是挂在 Release 上的 `MetasequoiaIME_Setup_v<版本>.exe`，并携带与 Release 二进制匹配的 PDB 符号文件。见下面「CI 契约」
- **本地测试**：手工跑，用本机自签名证书，用来在自己机器上验证安装流程。本文其余部分讲的是这条

版本号不是固定的，由 `Prepare-PackageFiles.ps1` 的 `-TargetVersion` 决定，默认 `0.0.1`；正式发布时 CI 传入真实版本。

本仓库 **不包含** 正式代码签名证书、私钥、指纹，也不包含编译好的 EXE/DLL、词库或安装包。

签名脚本会在你自己的机器上生成并复用一张自签名测试证书。

## CI 契约

根 `.github/workflows/release.yml` **不加修改地调用本目录的 `Prepare-PackageFiles.ps1` 和 `Compile-Installer.ps1`**。源目录名是 `Prepare-PackageFiles.ps1` 的参数，不是写死在脚本里的字面量：默认值为 `windows/`、`server/`、`ui-html/` 和仓根授权文件；旧目录或自定义布局通过参数覆盖。回归检查同时执行默认路径和显式参数的打包。辅助码默认读取 `engine/helpcode/`，词库缓存位于 `MetasequoiaImeDict/out/`。

合仓后 release workflow 传的是本仓的目录名，辅助码随固定 Engine gitlink 检出，词库按产品锁下载到仓根缓存：

```powershell
pwsh -File ./Prepare-PackageFiles.ps1 -TargetVersion 1.2.3 -RepoRoot .. `
    -TsfDirectory windows -ServerDirectory server `
    -UiHtmlDirectory ui-html -NoticesDirectory .
```

由此产生几条约束：

- **改动 `Prepare-PackageFiles.ps1` 里那些 `Assert-PathExists` 的源路径，会直接弄坏发布流水线。** 它断言的源路径由 workflow 逐一准备，改了要同步改 `.github/workflows/release.yml`。目录名本身可以由调用方指定，但每个目录内部的相对路径（如 `build-release\bin\Release`）仍是硬契约
- **`THIRD_PARTY_NOTICES.txt` 从 `-NoticesDirectory` 指向的目录取，随安装包装到程序目录。** 合仓后这份声明覆盖整个产品、放在仓根，所以它和 `-TsfDirectory` 分成了两个参数。词库主体含 rime-ice（GPL-3.0）内容，其许可要求保留署名，所以这份文件缺失会让打包直接失败，而不是静默跳过
- **`Sign-PackageBinaries-Local.ps1` 和 `Sign-Installer-Local.ps1` 只用于本地验证，CI 不会调用它们。** 它们创建本机自签名证书并尝试写入受信任存储，正式发布走 workflow 里用仓库 secret 中真实证书的签名步骤
- 词库不再从相邻的 `MetasequoiaImeDict` 工作目录取，CI 从产品锁指定的 `dict-*` release 下载并校验 SHA256，再放到脚本期望的位置
- `windows-2025` runner 自带 Inno Setup 6.7.1，`Compile-Installer.ps1` 能自己找到 `ISCC.exe`。但它不带 `ChineseSimplified.isl`，`msime_setup.iss` 的 `[Languages]` 段依赖那个文件，所以 workflow 会在编译前按固定 revision 和校验和把它装进去

## 依赖

- Windows 10/11，PowerShell 7（`pwsh`）
- [Inno Setup](https://jrsoftware.org/isinfo.php) 6.6 或更高版本
- Windows SDK（提供 `signtool.exe`）
- 先初始化本仓 submodule，并完成 `windows/`、`server/` 的 Release 编译以及 `ui-html/` 设置页构建。Release 构建必须生成同目录 PDB；打包脚本会拒绝缺少匹配符号的产物（符号是否随包安装是另一回事，由 `-IncludeSymbols` 决定）。
- 在仓库根目录运行 `python scripts/product_lock.py fetch-dictionaries --staging-root .`，下载并验证产品锁中的词库。
- `engine/helpcode/helpcodes/` 是本仓的目录，普通检出即有，无需旧 HelpCode 仓库，也无需初始化 submodule。

## 本地打包路径

从 `installer/` 运行上面的带参数打包命令即可。`-RepoRoot` 指向 MSIME-Windows 根目录；目录不同时通过参数指定，无需编辑脚本中的复制路径。

## 一键测试

先准备上述构建依赖与锁定词库。然后在管理员 PowerShell 中（把本机测试证书写入受信任存储需要提升权限）：

```powershell
cd path\to\MSIME-Windows\installer
pwsh -File .\test.ps1
```

`test.ps1` 会按顺序做这些事：

1. 编译本仓 TSF、Server，并构建设置页；缺少组件入口则失败
2. `Prepare-PackageFiles.ps1` — 收集 `server_exe\`、`tsf_dll\`、`app_data\`，并把 `msime_setup.iss` 的版本写成 `0.0.1`
3. `Sign-PackageBinaries-Local.ps1` — 本机自签名包内 EXE/DLL
4. `Compile-Installer.ps1` — 编译出 `Output\MetasequoiaIME_Setup_v0.0.1.exe`
5. `Sign-Installer-Local.ps1` — 本机自签名安装包
6. `Install.ps1` — 启动安装程序

也可以按上面的顺序逐步运行。

### 三个入口的区别

三个入口都是 `Invoke-LocalTest.ps1` 的薄包装，只是开关不同：

| 脚本 | 词库 | PDB |
| --- | --- | --- |
| `test.ps1` | 含 | 不含 |
| `test-light.ps1` | 不含 | 不含 |
| `test-symbols.ps1`（加 `-Full` 则含词库） | 默认不含 | 含 |

**PDB 默认不进本地安装包。** 它们有 ~140 MB，而真正的 EXE/DLL 只有 ~20 MB，Inno Setup 还要对它们做固实 LZMA2 压缩，是本地打包耗时的大头。只有需要在装好的机器上直接做崩溃分析时才用 `test-symbols.ps1`。正式发布仍然带符号：`release.yml` 显式传 `-IncludeSymbols`。

### 增量编译

`Invoke-LocalTest.ps1` 调用的 `lcompile-release*.ps1` 是增量的：

- `build-release` / `build{32,64}-release` 已有 CMake 缓存时不再重跑 configure。Visual Studio 生成器把 ZERO_CHECK 接进了每个目标，`CMakeLists.txt` 变化时构建会自己重新生成。改了 preset 或换了工具链，用 `-Reconfigure` 强制重跑
- 只构建打包用的目标（`MetasequoiaImeServer` / `MetasequoiaImeTsf`），不构建那些打完包又被删掉的测试可执行文件。要连测试一起构建（比如为了跑 ctest）传 `-Target ALL_BUILD`
- `cmake --build` 带 `--parallel`

没有改动时整轮编译约 15 秒；如果比这慢很多，慢的多半是打包而不是编译。

## 本机测试证书

第一次运行签名脚本时，会在当前用户证书存储里创建：

`CN=Metasequoia IME Local Test Code Signing`

之后复用同一张证书。产物只适合本机或已导入该证书公钥的测试机，不要当作正式分发包。

想让另一台测试机信任这张证书，把公钥导出成 `.cer` 后在那台机器上执行：

```powershell
certutil -addstore Root MetasequoiaImeLocalTest.cer
certutil -addstore TrustedPublisher MetasequoiaImeLocalTest.cer
```

不要把 `.cer` / `.pfx` 提交进本仓库。
