#define MyAppName "TwinFile C++ 原生版"
#define MyAppVersion "3.1.12"
#ifndef MyDistDir
  #define MyDistDir AddBackslash(SourcePath) + "dist\TwinFile"
#endif

[Setup]
AppId={{6E96CE9B-ECF3-4C07-AF1B-6D90BB9BE8B4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=TwinFile
AppPublisherURL=https://github.com/350624/TwinFile
AppSupportURL=https://github.com/350624/TwinFile/issues
LicenseFile=LICENSE
DefaultDirName={autopf}\TwinFileCpp
DefaultGroupName={#MyAppName}
OutputDir=releases
OutputBaseFilename=TwinFile-3.1-Cpp-Setup
VersionInfoVersion=3.1.12.0
SetupIconFile=assets\app.ico
UninstallDisplayIcon={app}\TwinFile.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
CloseApplications=yes
CloseApplicationsFilter=TwinFile.exe
RestartApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "chs"; MessagesFile: "assets\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "{#MyDistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion createallsubdirs

[Icons]
Name: "{group}\TwinFile C++"; Filename: "{app}\TwinFile.exe"
Name: "{autodesktop}\TwinFile C++"; Filename: "{app}\TwinFile.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TwinFile.exe"; Description: "运行 TwinFile C++ 原生版"; Flags: nowait postinstall skipifsilent unchecked
