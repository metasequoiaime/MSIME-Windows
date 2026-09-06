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
; 本仓库不包含任何预置代码签名证书。

#define MyAppName      "Metasequoia IME 水杉输入法"
#define MyAppVersion   "0.0.1"
#define MyAppPublisher "Metasequoia"
#define MyAppExeName   "MetasequoiaImeServer.exe"
#define MySettingsExeName "MetasequoiaImeSettings.exe"
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
; 安装向导中显示 GPLv3 正文。Prepare-PackageFiles.ps1 会把仓库根的 LICENSE 拷成这个文件。
LicenseFile={#MySourceRoot}\LICENSE.txt
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
Name: "{localappdata}\metasequoiaime"
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
Source: "{#MySourceRoot}\tsf_dll\32\*.dll"; \
    DestDir: "{commonpf32}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver 32bit

Source: "{#MySourceRoot}\tsf_dll\64\*.dll"; \
    DestDir: "{commonpf64}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver

Source: "{#MySourceRoot}\tsf_dll\32\*.pdb"; \
    DestDir: "{commonpf32}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion

Source: "{#MySourceRoot}\tsf_dll\64\*.pdb"; \
    DestDir: "{commonpf64}\metasequoiaime\{code:GetVersionDir}"; \
    Flags: ignoreversion

Source: "{#MySourceRoot}\server_exe\*"; \
    DestDir: "{commonpf64}\metasequoiaime\server"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

#ifdef LightPackage
; 轻量包只覆盖前端 HTML，不带词库/辅助码/出厂配置。
Source: "{#MySourceRoot}\app_data\html\*"; \
    DestDir: "{localappdata}\metasequoiaime\html"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
#else
; 包内故意不带 config.toml。通配复制再排除一次，防止以后又把用户配置打进包内。
Source: "{#MySourceRoot}\app_data\*"; DestDir: "{localappdata}\metasequoiaime"; \
    Excludes: "\config.toml,\config.base.toml,\config.default.toml"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall

; 用户配置只在首次安装时从出厂模板生成。升级时绝不覆盖已有 config.toml；
; Server 启动时再以 config.default.toml 合并：保留用户改过的值，带入新版新增项。
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{localappdata}\metasequoiaime"; DestName: "config.toml"; \
    Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{localappdata}\metasequoiaime"; DestName: "config.default.toml"; \
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

[Code]
var
  VersionDirName: String;
  NetworkPage: TInputOptionWizardPage;
  CloudCandidatesIndex: Integer;
  UserConfigExistedBeforeInstall: Boolean;

function UserConfigPath: String;
begin
  Result := ExpandConstant('{localappdata}\metasequoiaime\config.toml');
end;

{ 云候选是唯一一个装完就会联网的功能：输入过程中把当前拼写发给 Google 的 input-tools 服务。
  出厂默认开启，而安装器此前没有任何一屏提到过它，用户要读文档才会知道。这一页把它摆到安装
  过程里，选择写进首次生成的 config.toml。

  升级时跳过：那时 config.toml 已经属于用户，安装器不该替他重新决定。}
procedure InitializeWizard;
begin
#ifndef LightPackage
  UserConfigExistedBeforeInstall := FileExists(UserConfigPath);
  NetworkPage := CreateInputOptionPage(
    wpLicense,
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
begin
  Result := True;
  if (CurPageID = wpFinished) and (not WizardSilent) then
    LaunchInstalledComponents;
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
  Result :=
    IsUserDatabaseFile(FileName) or
    IsUserConfigFile(FileName) or
    IsUserSkinDirectory(FileName);
end;

function InitializeUninstall(): Boolean;
begin
  RegQueryStringValue(
    HKLM,
    'Software\Metasequoia\MetasequoiaIME',
    'VersionDir',
    VersionDirName
  );
  Result := True;
end;

procedure StopProcess(const ImageName: String);
var
  ResultCode: Integer;
begin
  { Watchdog 必须先停，否则它可能在卸载期间重新启动 Server。}
  Exec(
    ExpandConstant('{sys}\taskkill.exe'),
    '/F /T /IM "' + ImageName + '"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
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

procedure CleanAppDataExceptUserFiles;
var
  AppDataPath: String;
  FindRec: TFindRec;
  ItemPath: String;
begin
  AppDataPath := ExpandConstant('{localappdata}\metasequoiaime');
  if not DirExists(AppDataPath) then
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
  AppDataPath := ExpandConstant('{localappdata}\metasequoiaime');
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
  DataPath := ExpandConstant('{localappdata}\metasequoiaime');
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
#ifndef LightPackage
var
  FailedPath: String;
#endif
begin
  { 先锁定本次目录名，再清理能够释放的旧版本 DLL。}
  VersionDirName := GetVersionDir('');
  StopProcess('{#MyWatchdogName}');
  StopProcess('{#MyAppExeName}');
#ifdef LightPackage
  { 轻量包不替换词库：只清 HTML 和 Server/TSF，保留本机 msime.db 等。}
  TryDeleteTree(ExpandConstant('{localappdata}\metasequoiaime\html'));
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
#ifndef LightPackage
    ReplayUserDictionary;
    ApplyNetworkChoiceToUserConfig;
#endif
    CreateWatchdogLogonTask;
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
    StopProcess('{#MyWatchdogName}');
    StopProcess('{#MyAppExeName}');
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
    TryDeleteTree(ExpandConstant('{localappdata}\metasequoiaime'));
    TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime'));
  end;
end;
