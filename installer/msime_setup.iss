; Metasequoia IME — Inno Setup script
; 源文件根目录：本脚本所在目录
;
; 编译方法：
;   1. 安装 Inno Setup 6.6 或更高版本：https://jrsoftware.org/isinfo.php
;   2. 运行 .\Compile-Installer.ps1（或用 Inno Setup Compiler 打开本文件并按 Ctrl+F9）
;   输出安装包默认在：Output\
;
; 本地测试打包顺序：
;   1. Prepare-PackageFiles.ps1         收集本版本的安装文件
;   2. Sign-PackageBinaries-Local.ps1   用本机自签名测试证书给包内 EXE/DLL 签名
;   3. 本文件用 Inno Setup 编译
;   4. Sign-Installer-Local.ps1         用同一张本机测试证书给安装包签名
;
; 也可以直接运行 .\test.ps1 走完整测试流程。
; 只改 TSF / Server / HTML 时用 .\test-light.ps1：ISCC /DLightPackage=1，
; 打出不含词库的轻量包，安装时也不会删本机已有词库。
; 上面两条都不把 PDB 打进包；要带符号用 .\test-symbols.ps1。
; 本仓库不包含任何预置代码签名证书。

#define MyAppName      "Metasequoia IME 水杉输入法"
#define MyAppVersion   "0.0.1"
#define MyAppPublisher "Metasequoia"
#define MyAppExeName   "MetasequoiaImeServer.exe"
#define MySettingsExeName "MetasequoiaImeSettings.exe"
; 与 settings_app.cpp / settings_launcher.cpp 的 kQuitSettings 保持一致：WM_APP + 5。
#define MySettingsWindowClass "MetasequoiaImeSettingsWindow"
#define MySettingsQuitMessage 32773
#define MyEmojiPanelExeName "MetasequoiaImeEmojiPanel.exe"
#define MyKeyboardPanelExeName "MetasequoiaImeKeyboardPanel.exe"
#define MyHandwritingPanelExeName "MetasequoiaImeHandwritingPanel.exe"
#define MyWatchdogName "MetasequoiaImeWatchdog.exe"
#define MyWatchdogTaskName "Metasequoia IME Watchdog"
#define MyReplayName   "MetasequoiaImeDictionaryReplay.exe"
#define MyVersionDirBase "msime_v" + MyAppVersion
#define MySourceRoot   "."
#ifdef LightPackage
#define MyOutputSuffix "_light"
#else
#define MyOutputSuffix ""
#endif

[Setup]
AppId={{A7C3E91F-4B2D-4E8A-9F1C-6D5E8B0A2C4D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\metasequoiaime
DefaultGroupName={#MyAppName}
DisableDirPage=yes
; DisableDirPage=yes 时就绪页默认不显示目标目录，显式打开以便用户确认装到哪。
AlwaysShowDirOnReadyPage=yes
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=MetasequoiaIME_Setup_v{#MyAppVersion}{#MyOutputSuffix}
SetupIconFile={#MySourceRoot}\MetasequoiaIME.ico
Compression=lzma2
SolidCompression=yes
; 安装和卸载界面自动跟随 Windows 的浅色/深色模式。
WizardStyle=modern dynamic
PrivilegesRequired=admin
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={commonpf64}\metasequoiaime\MetasequoiaIME.ico
VersionInfoVersion={#MyAppVersion}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{commonpf32}\metasequoiaime\{code:GetVersionDir}"
Name: "{commonpf64}\metasequoiaime\{code:GetVersionDir}"
Name: "{commonpf64}\metasequoiaime\server"
; 用户数据（词库、配置、皮肤、前端资源）。默认在 LocalAppData，安装时可以改到别的盘，
; 选择写进 HKLM 的 DataDir，Server / TSF DLL / 引擎三方都从那里读。
Name: "{code:GetDataDir}"; Permissions: users-modify
; 用户自己的辅助码方案放这里，Server 扫描后列进设置页，升级时保留。
Name: "{code:GetDataDir}\helpcodes\custom"
; WebView2 子进程是中完整性，写不进内置 Administrator 的高完整性 LocalAppData。
Name: "{commonappdata}\metasequoiaime"
Name: "{commonappdata}\metasequoiaime\webview2"; Permissions: users-modify
Name: "{commonappdata}\metasequoiaime\webview2-settings"; Permissions: users-modify

[Files]
; 独立安装应用图标，供 Windows“已安装的应用”列表稳定显示。
Source: "{#MySourceRoot}\MetasequoiaIME.ico"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; 第三方声明随包安装。词库主体含 rime-ice（GPL-3.0）内容，其许可要求保留署名，
; 因此这份文件必须落到用户磁盘上，而不能只存在于源码仓库里。
Source: "{#MySourceRoot}\THIRD_PARTY_NOTICES.txt"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; GPLv3 第 4、6 条要求分发时向接收者提供许可证副本，而 THIRD_PARTY_NOTICES.txt 只是指向
; "the LICENSE file"、本身不含 GPL 正文。macOS 与 Linux 的 CMake 安装规则早已随包装入许可证，
; Windows 是唯一大规模分发却漏掉这一步的平台。
Source: "{#MySourceRoot}\LICENSE.txt"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; TSF DLL 使用版本独立目录，避免升级时覆盖仍被进程加载的 DLL。
; PDB 与对应 DLL 放在同一目录，调试器可按二进制的内嵌路径自动找到符号。
; 只有 Prepare-PackageFiles.ps1 -IncludeSymbols 才会把 PDB 放进 tsf_dll\；默认本地打包不含符号，
; 所以这两条必须带 skipifsourcedoesntexist，否则通配符匹配不到文件时 ISCC 会直接报错。
Source: "{#MySourceRoot}\tsf_dll\32\*.dll"; \
    DestDir: "{commonpf32}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver 32bit

Source: "{#MySourceRoot}\tsf_dll\64\*.dll"; \
    DestDir: "{commonpf64}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver

Source: "{#MySourceRoot}\tsf_dll\32\*.pdb"; \
    DestDir: "{commonpf32}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion skipifsourcedoesntexist

Source: "{#MySourceRoot}\tsf_dll\64\*.pdb"; \
    DestDir: "{commonpf64}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion skipifsourcedoesntexist

Source: "{#MySourceRoot}\server_exe\*"; \
    DestDir: "{commonpf64}\metasequoiaime\server"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

#ifdef LightPackage
; 轻量包只覆盖前端 HTML，不带词库/辅助码/出厂配置。
Source: "{#MySourceRoot}\app_data\html\*"; \
    DestDir: "{code:GetDataDir}\html"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
#else
; 包内故意不带 config.toml。通配复制再排除一次，防止以后又把用户配置打进包内。
Source: "{#MySourceRoot}\app_data\*"; DestDir: "{code:GetDataDir}"; \
    Excludes: "\config.toml,\config.base.toml,\config.default.toml"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall

; 用户配置只在首次安装时从出厂模板生成。升级时绝不覆盖已有 config.toml；
; Server 启动时再以 config.default.toml 合并：保留用户改过的值，带入新版新增项。
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{code:GetDataDir}"; DestName: "config.toml"; \
    Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{code:GetDataDir}"; DestName: "config.default.toml"; \
    Flags: ignoreversion uninsneveruninstall
#endif

[Icons]
Name: "{group}\{#MyAppName}"; \
    Filename: "{commonpf64}\metasequoiaime\server\{#MySettingsExeName}"; \
    WorkingDir: "{commonpf64}\metasequoiaime\server"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"

[Registry]
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "VersionDir"; ValueData: "{code:GetVersionDir}"; \
    Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "ServerPath"; \
    ValueData: "{commonpf64}\metasequoiaime\server\{#MyAppExeName}"; \
    Flags: uninsdeletevalue
; 用户数据目录的唯一权威来源。Server、TSF DLL 和引擎都按
; METASEQUOIA_IME_DATA_DIR → 这个键 → %LOCALAPPDATA%\metasequoiaime 的顺序解析；
; 32 位 TSF DLL 用 KEY_WOW64_64KEY 读，所以这里必须写在 64 位视图里
; （ArchitecturesInstallIn64BitMode 已经保证了这一点）。
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "DataDir"; ValueData: "{code:GetDataDir}"; \
    Flags: uninsdeletevalue

[Code]
const
  { 放在数据目录里，标记「这个目录是安装器建的」。覆盖安装和卸载只有看到它才敢
    整目录清理——用户可能把数据目录指到一个本来就有自己文件的文件夹。}
  DataDirMarkerName = '.metasequoiaime-data';

  { WebView2 Evergreen Runtime 在 EdgeUpdate 里的固定客户端 ID。}
  WebView2ClientKey =
    'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  { 安装器界面本身就是中文（InitializeSetup 在选语言对话框之前跑，拿不到用户选的语言），
    下载页也统一给简体中文版，省得用户落到英文页上再自己切。}
  WebView2DownloadUrl =
    'https://developer.microsoft.com/zh-cn/microsoft-edge/webview2';
  VCRuntimeKey = 'Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  VCRuntimeDownloadUrl =
    'https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist?view=msvc-170';

var
  VersionDirName: String;
  DataDirValue: String;
  PreviousDataDir: String;
  DataDirPage: TInputDirWizardPage;
  NetworkPage: TInputOptionWizardPage;
  CloudCandidatesIndex: Integer;
  UserConfigExistedBeforeInstall: Boolean;

{ WebView2 Runtime 与 VC 运行库都不随包分发：前者有自己的 Evergreen 更新通道，
  后者是系统级共享组件，安装器不该替用户装。但缺了任何一个，输入法装完就是坏的，
  而故障现场在安装结束之后——Server 直接起不来，或者候选窗一片空白，用户看不到
  任何解释。所以在安装开始前查一遍，把原因当场说清楚。

  返回空串表示没装；否则是 pv 里的版本号。}
function ReadWebView2Version: String;
begin
  Result := '';
  { 机器级安装由 32 位的 EdgeUpdate 写入，键落在 WOW6432Node 下。本安装器跑在
    64 位模式，HKLM 默认就是 64 位视图，所以主查 HKLM32；HKLM64 作为后备，
    以防将来的 ARM64 / 原生 64 位布局换了位置。用户级安装写在 HKCU。
    卸载没清干净的机器上 pv 可能残留成 0.0.0.0。}
  if not RegQueryStringValue(HKLM32, WebView2ClientKey, 'pv', Result) then
    if not RegQueryStringValue(HKLM64, WebView2ClientKey, 'pv', Result) then
      if not RegQueryStringValue(HKCU, WebView2ClientKey, 'pv', Result) then
        Result := '';
  Result := Trim(Result);
  if Result = '0.0.0.0' then
    Result := '';
end;

// 用注册表判定，而不是去 System32 里找 vcruntime140.dll：Setup.exe 是 32 位进程，
// Pascal Script 的 FileExists 会被 WOW64 重定向到 SysWOW64（[Files] 和 Exec 里的
// {sys} 由 Inno 自己处理，脚本函数不管），结果查的是 x86 运行库——只装了 x64 redist
// 的机器会被误判成缺少 x64 运行库。绕开重定向的两个办法都不可移植：
// EnableFsRedirection 在 Inno Setup 7 里已被移除，而它的替代品在 6.x 里还不存在；
// {sysnative} 对 64 位安装器又不可用。注册表访问没有这个问题，HKLM32 / HKLM64
// 两个视图都是显式指定的。
//
// x64 redist 的安装状态就记在这个键里。要求 14.20 以上（VC++ 2019 起）：server_exe
// 里每个 exe 都导入了 vcruntime140_1.dll，而它是从那一版才开始随 redist 分发的，
// 只装了 2015/2017 版的机器同样有 Installed=1，却照样起不来。
// 32 位 TSF DLL 用的是静态 CRT（windows/CMakeLists.txt 里的
// CMAKE_MSVC_RUNTIME_LIBRARY），所以不必检查 x86 redist。
// 注意：Inno Setup 的花括号注释不能嵌套，上面提到的常量会提前闭合 { } 注释，
// 所以这一段只能用行注释。
function VCRuntimeInstalledInView(RootKey: Integer): Boolean;
var
  Installed: Cardinal;
  Major: Cardinal;
  Minor: Cardinal;
begin
  Result :=
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Installed', Installed) and
    (Installed = 1) and
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Major', Major) and
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Minor', Minor) and
    ((Major > 14) or ((Major = 14) and (Minor >= 20)));
end;

{ redist 的安装包是 32 位的，键通常落在 WOW6432Node 下，但较新的版本两个视图都写，
  所以两边都查。}
function VCRuntimeIsInstalled: Boolean;
begin
  Result :=
    VCRuntimeInstalledInView(HKLM32) or VCRuntimeInstalledInView(HKLM64);
end;

{ 只用于写日志，不参与判定。}
function ReadVCRuntimeVersion: String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM32, VCRuntimeKey, 'Version', Result) then
    if not RegQueryStringValue(HKLM64, VCRuntimeKey, 'Version', Result) then
      Result := '';
  Result := Trim(Result);
end;

procedure OpenDownloadPage(const Url: String);
var
  ErrorCode: Integer;
begin
  { 安装器是提权进程，用它拉起的浏览器会以管理员身份运行；和 LaunchInstalledComponents
    一样交还给原用户。}
  ShellExecAsOriginalUser(
    'open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

function InitializeSetup: Boolean;
var
  WebView2Version: String;
  NeedsWebView2: Boolean;
  NeedsVCRuntime: Boolean;
  Prompt: String;
begin
  Result := True;

  WebView2Version := ReadWebView2Version;
  NeedsWebView2 := WebView2Version = '';
  if NeedsWebView2 then
    Log('Prerequisite missing: Microsoft Edge WebView2 Runtime.')
  else
    Log('WebView2 Runtime found: ' + WebView2Version);

  NeedsVCRuntime := not VCRuntimeIsInstalled;
  if NeedsVCRuntime then
    Log('Prerequisite missing: Visual C++ 2015-2022 Redistributable (x64).')
  else
    Log('Visual C++ runtime found: ' + ReadVCRuntimeVersion);

  if not (NeedsWebView2 or NeedsVCRuntime) then
    exit;

  Prompt := '这台电脑缺少输入法运行所需的系统组件：' + #13#10;
  if NeedsVCRuntime then
    Prompt := Prompt + #13#10 +
      '● Visual C++ 2015-2022 可再发行组件包（x64）' + #13#10 +
      '   缺少它输入法主程序根本无法启动（提示找不到 VCRUNTIME140.dll 之类）。' + #13#10 +
      '   下载：' + VCRuntimeDownloadUrl + #13#10 +
      '   直链：https://aka.ms/vs/17/release/vc_redist.x64.exe' + #13#10;
  if NeedsWebView2 then
    Prompt := Prompt + #13#10 +
      '● Microsoft Edge WebView2 Runtime' + #13#10 +
      '   缺少它候选窗口、设置页和表情 / 手写 / 键盘面板都打不开。' + #13#10 +
      '   下载：' + WebView2DownloadUrl + #13#10;
  Prompt := Prompt + #13#10 +
    '点击「是」打开下载页面并结束本次安装，装好组件后重新运行本安装程序；' + #13#10 +
    '点击「否」继续安装（装完仍需自行补齐上述组件，输入法才能正常工作）。';

  { 静默安装（CI、企业批量部署）不该被一个对话框卡住：默认继续，缺什么已经写进日志了。}
  if
    SuppressibleMsgBox(Prompt, mbConfirmation, MB_YESNO, IDNO) = IDYES
  then
  begin
    if NeedsVCRuntime then
      OpenDownloadPage(VCRuntimeDownloadUrl);
    if NeedsWebView2 then
      OpenDownloadPage(WebView2DownloadUrl);
    Result := False;
  end;
end;

{ 上一次安装（或历史版本）的数据目录。没有注册表值就是历史默认位置。}
function ResolvePreviousDataDir: String;
var
  Recorded: String;
begin
  if PreviousDataDir = '' then
  begin
    Recorded := '';
    if
      RegQueryStringValue(
        HKLM,
        'Software\Metasequoia\MetasequoiaIME',
        'DataDir',
        Recorded
      ) and (Trim(Recorded) <> '')
    then
      PreviousDataDir := RemoveBackslashUnlessRoot(Trim(Recorded))
    else
      PreviousDataDir := ExpandConstant('{localappdata}\metasequoiaime');
  end;
  Result := PreviousDataDir;
end;

{ 本次安装要用的数据目录。静默安装可以用 /DATADIR="D:\..." 指定；
  两者都没有时沿用上一次的位置。}
function GetDataDir(Param: String): String;
var
  FromCommandLine: String;
begin
  if DataDirValue = '' then
  begin
    FromCommandLine := Trim(ExpandConstant('{param:DATADIR|}'));
    if FromCommandLine <> '' then
      DataDirValue := RemoveBackslashUnlessRoot(FromCommandLine)
    else
      DataDirValue := ResolvePreviousDataDir;
  end;
  Result := DataDirValue;
end;

function DataDirMarkerPath(const Directory: String): String;
begin
  Result := AddBackslash(Directory) + DataDirMarkerName;
end;

{ 只有这两种目录允许整目录清理：带标记的（我们建的），
  以及历史默认位置（老版本装的，那时还没有标记文件）。}
function OwnsDataDir(const Directory: String): Boolean;
begin
  Result :=
    (Directory <> '') and
    (FileExists(DataDirMarkerPath(Directory)) or
     (CompareText(
        Directory,
        ExpandConstant('{localappdata}\metasequoiaime')) = 0));
end;

procedure WriteDataDirMarker(const Directory: String);
var
  Lines: TArrayOfString;
begin
  if FileExists(DataDirMarkerPath(Directory)) then
    exit;
  SetArrayLength(Lines, 1);
  Lines[0] := 'Metasequoia IME user data directory.';
  SaveStringsToFile(DataDirMarkerPath(Directory), Lines, False);
end;

function UserConfigPath: String;
begin
  Result := AddBackslash(GetDataDir('')) + 'config.toml';
end;

function IsPathInside(const Child, Parent: String): Boolean;
begin
  Result :=
    (CompareText(Child, Parent) = 0) or
    (CompareText(
       Copy(AddBackslash(Child), 1, Length(AddBackslash(Parent))),
       AddBackslash(Parent)) = 0);
end;

function DirectoryIsEmpty(const Directory: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if not DirExists(Directory) then
    exit;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Result := False;
          exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 返回空串表示这个路径可以用；否则返回要展示给用户的原因。}
function DataDirRejectionReason(const Directory: String): String;
var
  Critical: array[0..6] of String;
  Index: Integer;
  ProbePath: String;
begin
  Result := '';

  if (Length(Directory) < 4) or (Directory[2] <> ':') or (Directory[3] <> '\') then
  begin
    Result := '请填写本机磁盘上的完整路径，例如 D:\MetasequoiaIME。';
    exit;
  end;
  if not DirExists(Copy(Directory, 1, 3)) then
  begin
    Result := '找不到驱动器 ' + Copy(Directory, 1, 2) + '，请换一个位置。';
    exit;
  end;
  if CompareText(RemoveBackslashUnlessRoot(Directory), Copy(Directory, 1, 2)) = 0 then
  begin
    Result := '不能直接使用驱动器根目录，请指定一个子目录。';
    exit;
  end;

  { 覆盖安装会清理数据目录里的旧资源，卸载会整个删掉它。
    因此它既不能落在程序目录里，也不能反过来包住系统或用户的关键目录。}
  if
    IsPathInside(Directory, ExpandConstant('{commonpf64}\metasequoiaime')) or
    IsPathInside(Directory, ExpandConstant('{commonpf32}\metasequoiaime'))
  then
  begin
    Result := '数据目录不能放在输入法的程序目录里面。';
    exit;
  end;

  Critical[0] := ExpandConstant('{win}');
  Critical[1] := ExpandConstant('{commonpf64}');
  Critical[2] := ExpandConstant('{commonpf32}');
  Critical[3] := ExpandConstant('{localappdata}');
  Critical[4] := ExpandConstant('{userappdata}');
  { Inno 没有对应用户主目录的常量，用环境变量展开。取不到时下面的空值检查会跳过这一项。}
  Critical[5] := ExpandConstant('{%USERPROFILE|}');
  Critical[6] := ExpandConstant('{commonappdata}');
  for Index := 0 to 6 do
  begin
    if (Critical[Index] <> '') and IsPathInside(Critical[Index], Directory) then
    begin
      Result :=
        '这个目录包含了系统或用户的重要目录（' + Critical[Index] + '），' +
        '卸载时会连它一起删除。请另选一个专用目录。';
      exit;
    end;
  end;

  if not ForceDirectories(Directory) then
  begin
    Result := '无法创建目录 ' + Directory + '，请检查权限或换一个位置。';
    exit;
  end;
  ProbePath := AddBackslash(Directory) + 'msime-write-probe.tmp';
  if not SaveStringToFile(ProbePath, 'probe', False) then
  begin
    Result := '目录 ' + Directory + ' 不可写，请换一个位置。';
    exit;
  end;
  DeleteFile(ProbePath);
end;

{ CreateInputDirPage 自带的浏览按钮调用 BrowseForFolder 时不给新建文件夹按钮，用户没法在对话框里
  当场建一个目录。接管它的 OnClick，换成带新建按钮的那种对话框。}
procedure DataDirBrowseClick(Sender: TObject);
var
  Chosen: String;
begin
  Chosen := Trim(DataDirPage.Values[0]);
  if BrowseForFolder('请选择输入法数据的存放位置：', Chosen, True) then
    DataDirPage.Values[0] := Chosen;
end;

{ 云候选是唯一一个装完就会联网的功能：输入过程中把当前拼写发给 Google 的 input-tools 服务。
  出厂默认开启，而安装器此前没有任何一屏提到过它，用户要读文档才会知道。这一页把它摆到安装
  过程里，选择写进首次生成的 config.toml。

  升级时跳过：那时 config.toml 已经属于用户，安装器不该替他重新决定。}
procedure InitializeWizard;
begin
  { 词库、用户配置、皮肤和前端资源都在这个目录下，整包有几百 MB，
    所以要让用户能把它放到别的盘。程序本体仍然装在 Program Files：
    Server 带 uiAccess=true，只有装在受信任目录里这个标志才生效。}
  DataDirPage := CreateInputDirPage(
    wpLicense,
    '选择数据位置',
    '输入法数据存放在哪里',
    '请选择输入法数据（词库、配置、皮肤）的存放位置。',
    True,
    'metasequoiaime'
  );
  DataDirPage.Add('');
  DataDirPage.Values[0] := ResolvePreviousDataDir;
  DataDirPage.Buttons[0].OnClick := @DataDirBrowseClick;

#ifndef LightPackage
  UserConfigExistedBeforeInstall := FileExists(UserConfigPath);
  NetworkPage := CreateInputOptionPage(
    DataDirPage.ID,
    '联网功能',
    '选择安装后哪些功能可以联网',
    '拼音切分、候选排序和词频学习全部在本机完成，不联网。' + #13#10 +
    '下面这一项是唯一一个装完就会生效的联网功能。AI 联想、候选翻译、语音输入都需要你自己填入 API token 之后才会发出任何请求。' + #13#10#13#10 +
    '安装后随时可以在「设置 → 输入」里改变这个选择。',
    False,
    False
  );
  CloudCandidatesIndex := NetworkPage.Add(
    '启用云候选：输入过程中把当前正在输入的拼写通过 HTTPS 发送给 Google 的 input-tools 服务' +
    '（inputtools.google.com），换回一条额外候选。已上屏的文本、词库内容和学习到的词频都不会发送。');
  NetworkPage.Values[CloudCandidatesIndex] := True;
#endif
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (NetworkPage <> nil) and (PageID = NetworkPage.ID) then
    Result := UserConfigExistedBeforeInstall;
end;

{ 只在本次安装刚生成 config.toml 时写入，且只改 [general] 段里的这一个键。找不到就什么都不做——
  这一步失败不应该让安装失败。}
procedure ApplyNetworkChoiceToUserConfig;
var
  Lines: TArrayOfString;
  Index: Integer;
  Trimmed: String;
  InGeneral: Boolean;
begin
  if UserConfigExistedBeforeInstall or (NetworkPage = nil) then
    Exit;
  if NetworkPage.Values[CloudCandidatesIndex] then
    Exit;
  if not LoadStringsFromFile(UserConfigPath, Lines) then
    Exit;

  InGeneral := False;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Trimmed := Trim(Lines[Index]);
    if (Length(Trimmed) > 0) and (Trimmed[1] = '[') then
      InGeneral := (Trimmed = '[general]')
    else if InGeneral and (Pos('cloud_candidates', Trimmed) = 1) then
    begin
      Lines[Index] := 'cloud_candidates = false';
      SaveStringsToFile(UserConfigPath, Lines, False);
      Exit;
    end;
  end;
end;

procedure LaunchInstalledComponents;
var
  ErrorCode: Integer;
begin
  { uiAccess=true 的程序不能用 CreateProcess 拉起（会报 740）。
    完成页点击 Finish 后，以原用户身份执行 ShellExecute（等同双击）。}
  ShellExecAsOriginalUser(
    '',
    ExpandConstant('{commonpf64}\metasequoiaime\server\{#MyAppExeName}'),
    '',
    '',
    SW_SHOWNORMAL,
    ewNoWait,
    ErrorCode
  );
  ShellExecAsOriginalUser(
    '',
    ExpandConstant('{commonpf64}\metasequoiaime\server\{#MyWatchdogName}'),
    '',
    '',
    SW_SHOWNORMAL,
    ewNoWait,
    ErrorCode
  );
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Chosen: String;
  Reason: String;
begin
  Result := True;

  if (DataDirPage <> nil) and (CurPageID = DataDirPage.ID) then
  begin
    Chosen := RemoveBackslashUnlessRoot(Trim(DataDirPage.Values[0]));
    Reason := DataDirRejectionReason(Chosen);
    if Reason <> '' then
    begin
      MsgBox(Reason, mbError, MB_OK);
      Result := False;
      exit;
    end;
    { 用户可能指到一个本来就有东西的文件夹。装进去没问题（清理和卸载都认标记文件），
      但得先说清楚里面的既有文件不归输入法管。}
    if
      (not DirectoryIsEmpty(Chosen)) and
      (not OwnsDataDir(Chosen)) and
      (MsgBox(
         '目录 ' + Chosen + ' 里已经有其他文件。' + #13#10 +
         '输入法会在其中创建自己的文件，不会动你原有的内容，卸载时也只删除自己的部分。' + #13#10 + #13#10 +
         '确定使用这个目录吗？',
         mbConfirmation, MB_YESNO) <> IDYES)
    then
    begin
      Result := False;
      exit;
    end;
    DataDirValue := Chosen;
    exit;
  end;

  if (CurPageID = wpFinished) and (not WizardSilent) then
    LaunchInstalledComponents;
end;

function UpdateReadyMemo(
  Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine +
    '数据目录（词库、用户配置、皮肤）：' + NewLine + Space + GetDataDir('');
  if CompareText(GetDataDir(''), ResolvePreviousDataDir) <> 0 then
    Result := Result + NewLine + NewLine +
      '现有数据将从这里迁移：' + NewLine + Space + ResolvePreviousDataDir;
end;

function GetVersionDir(Param: String): String;
var
  Candidate: String;
  Suffix: Integer;
begin
  if VersionDirName = '' then
  begin
    Candidate := '{#MyVersionDirBase}';
    Suffix := 0;
    while
      DirExists(ExpandConstant(
        '{commonpf32}\metasequoiaime\' + Candidate)) or
      DirExists(ExpandConstant(
        '{commonpf64}\metasequoiaime\' + Candidate))
    do
    begin
      Suffix := Suffix + 1;
      Candidate := '{#MyVersionDirBase}.' + IntToStr(Suffix);
    end;
    VersionDirName := Candidate;
  end;
  Result := VersionDirName;
end;

function IsUserDatabaseFile(const FileName: String): Boolean;
begin
  { WAL 中可能还有尚未 checkpoint 的用户操作，必须与主库一起保留。}
  Result :=
    (CompareText(FileName, 'msime_user.db') = 0) or
    (CompareText(FileName, 'msime_user.db-wal') = 0) or
    (CompareText(FileName, 'msime_user.db-shm') = 0) or
    (CompareText(FileName, 'msime_user.db-journal') = 0);
end;

function IsUserConfigFile(const FileName: String): Boolean;
begin
  { config.toml 是用户配置，config.base.toml 是上次合并用的模板基线：
    没有它，Server 就无法判断某一项到底是用户改的还是旧版默认值。}
  Result :=
    (CompareText(FileName, 'config.toml') = 0) or
    (CompareText(FileName, 'config.base.toml') = 0);
end;

function IsUserSkinDirectory(const FileName: String): Boolean;
begin
  { 外部皮肤在 %LOCALAPPDATA%\metasequoiaime\skins，升级安装不得清掉。}
  Result := CompareText(FileName, 'skins') = 0;
end;

function IsPreservedAppDataItem(const FileName: String): Boolean;
begin
  { 标记文件也要留下。它虽然会在 ssPostInstall 重写一遍，但安装若在中途失败，
    没有它的数据目录就不再被认作我们建的，后续的清理和卸载都会跳过。}
  Result :=
    IsUserDatabaseFile(FileName) or
    IsUserConfigFile(FileName) or
    IsUserSkinDirectory(FileName) or
    (CompareText(FileName, DataDirMarkerName) = 0);
end;

function InitializeUninstall(): Boolean;
begin
  RegQueryStringValue(
    HKLM,
    'Software\Metasequoia\MetasequoiaIME',
    'VersionDir',
    VersionDirName
  );
  { uninsdeletevalue 会在卸载过程中删掉 DataDir，所以要先读出来缓存住。}
  ResolvePreviousDataDir;
  Result := True;
end;

procedure StopProcess(const ImageName: String);
var
  ResultCode: Integer;
begin
  Exec(
    ExpandConstant('{sys}\taskkill.exe'),
    '/F /T /IM "' + ImageName + '"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure StopSettingsProcess;
var
  SettingsWindow: HWND;
  WaitedMs: Integer;
begin
  // 设置窗口关闭后只是隐藏，进程还要驻留十分钟保住已导航完的 WebView2。
  // 覆盖安装要删掉整个 server 目录，所以这里必须把它请走：先投 kQuitSettings
  // 让它自己收尾（Server 已被强杀，没人替我们发这条消息了），再强杀兜底。
  SettingsWindow := FindWindowByClassName('{#MySettingsWindowClass}');
  if SettingsWindow <> 0 then
  begin
    PostMessage(SettingsWindow, {#MySettingsQuitMessage}, 0, 0);
    WaitedMs := 0;
    while (WaitedMs < 3000) and
          (FindWindowByClassName('{#MySettingsWindowClass}') <> 0) do
    begin
      Sleep(100);
      WaitedMs := WaitedMs + 100;
    end;
  end;
  StopProcess('{#MySettingsExeName}');
end;

procedure StopImeProcesses;
begin
  { Watchdog 先停，否则它会在我们删文件期间把 Server 拉起来。}
  StopProcess('{#MyWatchdogName}');
  StopProcess('{#MyAppExeName}');
  StopSettingsProcess;
  { 面板通常是 Server 的子进程、随 /T 一起走；Server 若已崩溃它们会变成孤儿，
    同样占着 server 目录里的 exe，所以显式再收一遍。}
  StopProcess('{#MyEmojiPanelExeName}');
  StopProcess('{#MyKeyboardPanelExeName}');
  StopProcess('{#MyHandwritingPanelExeName}');
end;

procedure DeleteWatchdogLogonTask;
var
  ResultCode: Integer;
begin
  { /F makes this idempotent when upgrading from a build without the task. }
  Exec(
    ExpandConstant('{sys}\schtasks.exe'),
    '/Delete /F /TN "{#MyWatchdogTaskName}"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure EnsureImeUserDataDir;
var
  AppDataPath: String;
  ResultCode: Integer;
begin
  // Elevated setup writes {localappdata} as high integrity. Medium-IL Server
  // and Settings cannot replace those files. Users who never rewrote config.toml
  // at the real path (the non-ASCII path bug) keep that leftover and cannot save.
  // Note: brace comments do not nest in Inno Setup, so a constant like the one
  // above would close a { } comment early -- keep these as line comments.
  // Also required when the data directory sits on another volume: files the elevated setup
  // copied there inherit the parent's ACL, which may not let a Medium-IL Server write them.
  AppDataPath := GetDataDir('');
  ForceDirectories(AppDataPath);
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + AppDataPath + '" /grant *S-1-5-32-545:(OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + AppDataPath + '" /setintegritylevel (OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure EnsureSharedWebView2DataDir;
var
  RootPath: String;
  ResultCode: Integer;
begin
  { Edge 子进程需要 Users 可写、中完整性的目录。安装器本身是高完整性，
    只 CreateDir 会带上高完整性标签，所以还要降完整性。 }
  RootPath := ExpandConstant('{commonappdata}\metasequoiaime');
  ForceDirectories(RootPath + '\webview2');
  ForceDirectories(RootPath + '\webview2-settings');
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + RootPath + '" /grant *S-1-5-32-545:(OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + RootPath + '" /setintegritylevel (OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure CreateWatchdogLogonTask;
var
  WatchdogPath: String;
  Params: String;
  ResultCode: Integer;
begin
  ResultCode := -1;
  WatchdogPath := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\{#MyWatchdogName}');
  { /F replaces the same fixed-name task during an upgrade. /IT keeps the
    task in the interactive user's session; LIMITED avoids an elevated token. }
  Params :=
    '/Create /F /TN "{#MyWatchdogTaskName}" /SC ONLOGON ' +
    '/RL LIMITED /IT /TR "\"' + WatchdogPath + '\""';
  if
    (not Exec(
      ExpandConstant('{sys}\schtasks.exe'),
      Params,
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode
    )) or
    (ResultCode <> 0)
  then
    RaiseException(
      '无法创建输入法登录启动任务（退出码：' +
      IntToStr(ResultCode) + '）。');
end;

procedure TryDeleteTree(const Path: String);
begin
  { 忽略返回值：被占用文件保留，其他能删除的文件仍继续清理。}
  DelTree(Path, True, True, True);
end;

{ helpcodes 里的内置表由本次安装重写，custom 子目录是用户自己放的辅助码，升级不得清掉。}
procedure CleanHelpcodesExceptCustom(const HelpcodesPath: String);
var
  FindRec: TFindRec;
  ItemPath: String;
begin
  if FindFirst(AddBackslash(HelpcodesPath) + '*', FindRec) then
  begin
    try
      repeat
        if
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (CompareText(FindRec.Name, 'custom') <> 0)
        then
        begin
          ItemPath := AddBackslash(HelpcodesPath) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            TryDeleteTree(ItemPath)
          else
            DeleteFile(ItemPath);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CleanAppDataExceptUserFiles;
var
  AppDataPath: String;
  FindRec: TFindRec;
  ItemPath: String;
begin
  AppDataPath := GetDataDir('');
  if not DirExists(AppDataPath) then
    exit;
  { 只清理我们自己建的目录。用户可能把数据目录指到一个本来就有文件的文件夹，
    那里除了输入法自己的文件之外的一切都不归我们删。}
  if not OwnsDataDir(AppDataPath) then
    exit;

  if FindFirst(AddBackslash(AppDataPath) + '*', FindRec) then
  begin
    try
      repeat
        if
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (not IsPreservedAppDataItem(FindRec.Name))
        then
        begin
          ItemPath := AddBackslash(AppDataPath) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
            DeleteFile(ItemPath)
          else if CompareText(FindRec.Name, 'helpcodes') = 0 then
            CleanHelpcodesExceptCustom(ItemPath)
          else
            TryDeleteTree(ItemPath);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function RemoveFileWithRetry(const Path: String): Boolean;
var
  Attempt: Integer;
begin
  for Attempt := 1 to 5 do
  begin
    if not FileExists(Path) then
    begin
      Result := True;
      exit;
    end;
    DeleteFile(Path);
    if not FileExists(Path) then
    begin
      Result := True;
      exit;
    end;
    Sleep(200);
  end;
  Result := not FileExists(Path);
end;

function RemoveOldTargetDatabaseFiles(var FailedPath: String): Boolean;
var
  AppDataPath: String;
  FileNames: array[0..11] of String;
  Index: Integer;
  Path: String;
begin
  AppDataPath := GetDataDir('');
  { 先删 sidecar；若仍被占用，可在动主库和其他应用数据前安全中止。}
  FileNames[0] := 'msime.db-wal';
  FileNames[1] := 'msime.db-shm';
  FileNames[2] := 'msime.db-journal';
  FileNames[3] := 'english.db-wal';
  FileNames[4] := 'english.db-shm';
  FileNames[5] := 'english.db-journal';
  FileNames[6] := 'others.db-wal';
  FileNames[7] := 'others.db-shm';
  FileNames[8] := 'others.db-journal';
  FileNames[9] := 'msime.db';
  FileNames[10] := 'english.db';
  FileNames[11] := 'others.db';

  for Index := 0 to 11 do
  begin
    Path := AddBackslash(AppDataPath) + FileNames[Index];
    if not RemoveFileWithRetry(Path) then
    begin
      FailedPath := Path;
      Result := False;
      exit;
    end;
  end;
  Result := True;
end;

procedure ReplayUserDictionary;
var
  ReplayPath: String;
  DataPath: String;
  ResultCode: Integer;
begin
  DataPath := GetDataDir('');
  if not FileExists(AddBackslash(DataPath) + 'msime_user.db') then
  begin
    Log('User dictionary replay skipped: msime_user.db does not exist.');
    exit;
  end;

  ReplayPath := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\{#MyReplayName}');
  Log('Starting user dictionary replay.');
  if not Exec(
    ReplayPath,
    '--data-dir "' + DataPath + '"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) then
    RaiseException(
      '无法启动用户词库回放程序。请确认安装文件完整后重试。');

  if ResultCode <> 0 then
    RaiseException(
      '用户词库回放失败（退出码：' + IntToStr(ResultCode) +
      '）。安装已停止，以避免启动未恢复用户词库的新版本。');
  Log('User dictionary replay completed successfully.');
end;

procedure TryDeleteOldVersionDirs(const RootPath: String);
var
  FindRec: TFindRec;
begin
  if FindFirst(
    AddBackslash(RootPath) + 'msime_v*',
    FindRec
  ) then
  begin
    try
      repeat
        if
          ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (CompareText(FindRec.Name, VersionDirName) <> 0)
        then
          TryDeleteTree(AddBackslash(RootPath) + FindRec.Name);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 用户改了数据目录：把上一处的用户数据搬过来。只搬真正属于用户、装不回来的东西——
  词库主体、前端资源和辅助码都会由本次安装重新写入新目录。
  用 robocopy 而不是 RenameFile：跨盘移动目录时 MoveFile 会直接失败。}
function MigrateUserDataDir(const OldDir, NewDir: String): String;
var
  ResultCode: Integer;
  Moved: Boolean;
begin
  Result := '';
  if (OldDir = '') or (CompareText(OldDir, NewDir) = 0) or (not DirExists(OldDir)) then
    exit;

  Log('Migrating user data from ' + OldDir + ' to ' + NewDir);
  ForceDirectories(NewDir);

  Moved := Exec(
    ExpandConstant('{sys}\robocopy.exe'),
    '"' + RemoveBackslashUnlessRoot(OldDir) + '" "' + RemoveBackslashUnlessRoot(NewDir) + '" ' +
    'msime_user.db msime_user.db-wal msime_user.db-shm msime_user.db-journal ' +
    'config.toml config.base.toml /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) and (ResultCode < 8);

  { 外部皮肤是用户自己放进来的，同样搬走。}
  if DirExists(AddBackslash(OldDir) + 'skins') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'skins" "' + AddBackslash(NewDir) + 'skins" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  { 自定义辅助码也是用户自己放进来的。}
  if DirExists(AddBackslash(OldDir) + 'helpcodes\custom') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'helpcodes\custom" "' + AddBackslash(NewDir) + 'helpcodes\custom" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  if not Moved then
  begin
    Log('User data migration reported failures; leaving ' + OldDir + ' in place.');
    { 用户词库没搬成必须让安装失败。否则新版本会对着一个空的 msime_user.db 启动，
      ssPostInstall 的回放只会记一句 skipped，用户的自造词和词频就这么静悄悄没了——
      这正是 ReplayUserDictionary 失败时也要中止安装的那条理由。
      其他文件（config.toml、皮肤）搬不动只记日志：它们丢了可以重建。}
    if FileExists(AddBackslash(OldDir) + 'msime_user.db') then
      Result :=
        '无法把用户词库从 ' + OldDir + ' 移动到 ' + NewDir + '。' + #13#10 +
        '请确认输入法相关进程已全部退出、目标磁盘可写且空间足够，然后重试安装。' + #13#10 +
        '你的用户词库仍留在 ' + OldDir + '，本次安装没有改动它。';
    exit;
  end;

  { 旧目录里剩下的是可重建的资源（词库、html、cache 等），只在确定是我们建的时候才整个删掉。}
  if OwnsDataDir(OldDir) then
    TryDeleteTree(OldDir);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  DataDirProblem: String;
#ifndef LightPackage
  FailedPath: String;
#endif
begin
  { 静默安装不会走向导页，/DATADIR= 传进来的值在这里才第一次被检查。}
  DataDirProblem := DataDirRejectionReason(GetDataDir(''));
  if DataDirProblem <> '' then
  begin
    Result := '数据目录 ' + GetDataDir('') + ' 不可用：' + DataDirProblem;
    exit;
  end;

  { 先锁定本次目录名，再清理能够释放的旧版本 DLL。}
  VersionDirName := GetVersionDir('');
  StopImeProcesses;
  { 进程停了才能动数据文件：msime_user.db 会被 Server 打开着。}
  DataDirProblem := MigrateUserDataDir(ResolvePreviousDataDir, GetDataDir(''));
  if DataDirProblem <> '' then
  begin
    Result := DataDirProblem;
    exit;
  end;
#ifdef LightPackage
  { 轻量包不替换词库：只清 HTML 和 Server/TSF，保留本机 msime.db 等。}
  TryDeleteTree(AddBackslash(GetDataDir('')) + 'html');
#else
  { 不能让旧 WAL/SHM 与即将复制的新主数据库混用。}
  if not RemoveOldTargetDatabaseFiles(FailedPath) then
  begin
    Result :=
      '无法删除旧词库文件：' + FailedPath + #13#10 +
      '它可能仍被输入法相关进程占用。请关闭相关程序后重试安装。';
    exit;
  end;
  { 目标词库已安全移除，再清理旧前端资源与 WebView2 用户数据。
    用户配置和外部皮肤目录在这里保留；webview2 目录故意重建，
    由 Server 冷启动路径保证 FTB/候选窗仍能稳定揭罩。}
  CleanAppDataExceptUserFiles;
#endif
  TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime\webview2'));
  TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime\webview2-settings'));
  TryDeleteTree(ExpandConstant(
    '{commonpf64}\metasequoiaime\server'));
  TryDeleteOldVersionDirs(ExpandConstant(
    '{commonpf32}\metasequoiaime'));
  TryDeleteOldVersionDirs(ExpandConstant(
    '{commonpf64}\metasequoiaime'));
  { 随后的 [Files] 与 ssPostInstall 会写入新 Server 和登录任务。}
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    { 先打标记，再调整权限：之后的覆盖安装和卸载靠它判断这个目录是不是我们建的。}
    WriteDataDirMarker(GetDataDir(''));
#ifndef LightPackage
    ReplayUserDictionary;
    ApplyNetworkChoiceToUserConfig;
#endif
    CreateWatchdogLogonTask;
    EnsureImeUserDataDir;
    EnsureSharedWebView2DataDir;
    { Keep the old autostart intact until its scheduled-task replacement has
      been created successfully, then remove the Explorer-delayed Run entry. }
    RegDeleteValue(
      HKLM,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'MetasequoiaImeWatchdog'
    );
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    DeleteWatchdogLogonTask;
    RegDeleteValue(
      HKLM,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'MetasequoiaImeWatchdog'
    );
    StopImeProcesses;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    TryDeleteTree(ExpandConstant(
      '{commonpf64}\metasequoiaime\server'));
    if VersionDirName <> '' then
    begin
      TryDeleteTree(ExpandConstant(
        '{commonpf32}\metasequoiaime\' + VersionDirName));
      TryDeleteTree(ExpandConstant(
        '{commonpf64}\metasequoiaime\' + VersionDirName));
    end;
    TryDeleteTree(ExpandConstant('{commonpf32}\metasequoiaime'));
    TryDeleteTree(ExpandConstant('{commonpf64}\metasequoiaime'));
    { 数据目录可能被用户指到了别的盘，甚至指到一个本来就有文件的文件夹：
      只有确认是安装器建的（带标记文件，或历史默认位置）才整个删除。}
    if OwnsDataDir(ResolvePreviousDataDir) then
      TryDeleteTree(ResolvePreviousDataDir);
    TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime'));
  end;
end;
