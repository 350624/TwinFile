#define MyAppName "TwinFile Downloader"
#define MyAppVersion "3.1.32"
#ifndef MyDistDir
  #define MyDistDir AddBackslash(SourcePath) + "dist\TwinFileDownloader"
#endif

[Setup]
AppId={{C3432405-1668-4E8F-9D46-758594F78171}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=TwinFile
AppPublisherURL=https://github.com/350624/TwinFile
AppSupportURL=https://github.com/350624/TwinFile/issues
DefaultDirName={localappdata}\Programs\TwinFile Downloader
DefaultGroupName=TwinFile Downloader
OutputDir=releases
OutputBaseFilename=TwinFile-Downloader-3.1.32-Windows-x64-Setup
VersionInfoVersion=3.1.32.0
SetupIconFile=assets\app.ico
UninstallDisplayIcon={app}\TwinFileDownloader.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
CloseApplications=yes
CloseApplicationsFilter=TwinFileDownloader.exe
RestartApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=LICENSE

[Languages]
Name: "chs"; MessagesFile: "assets\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "{#MyDistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\TwinFile Downloader"; Filename: "{app}\TwinFileDownloader.exe"
Name: "{autodesktop}\TwinFile Downloader"; Filename: "{app}\TwinFileDownloader.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TwinFileDownloader.exe"; Description: "运行 TwinFile Downloader"; Flags: nowait postinstall skipifsilent unchecked
