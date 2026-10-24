param(
    [string]$Compiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    [string]$QtRoot,
    [string]$CompilerRoot,
    [string]$CMake = 'cmake',
    [string]$ToolRoot = (Join-Path $PSScriptRoot 'tools'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$VersionMatch = [regex]::Match((Get-Content -LiteralPath "$PSScriptRoot\CMakeLists.txt" -Raw), 'project\(TwinFileNative VERSION (\d+\.\d+\.\d+)')
if (-not $VersionMatch.Success) { throw 'Cannot read project version' }
$Version = $VersionMatch.Groups[1].Value
if (-not $SkipBuild) {
    if (-not $QtRoot -or -not $CompilerRoot) { throw 'Pass -QtRoot and -CompilerRoot, or deploy first and use -SkipBuild' }
    & "$PSScriptRoot\build.ps1" -QtRoot $QtRoot -CompilerRoot $CompilerRoot -CMake $CMake -ToolRoot $ToolRoot -Package
}
if (-not (Test-Path -LiteralPath "$PSScriptRoot\dist\TwinFile\TwinFile.exe")) { throw 'Build the native portable package first' }
if (-not (Test-Path -LiteralPath $Compiler)) { throw 'Pass -Compiler with the full Inno Setup compiler path' }
& $Compiler "$PSScriptRoot\installer.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
$DownloaderInstaller = Join-Path $PSScriptRoot "releases\TwinFile-Downloader-$Version-Windows-x64-Setup.exe"
$DownloaderPackage = Join-Path $PSScriptRoot "dist\TwinFileDownloader-$Version"
if (-not (Test-Path -LiteralPath "$DownloaderPackage\TwinFileDownloader.exe")) { throw 'Build the standalone downloader package first' }
& $Compiler "/DMyDistDir=$DownloaderPackage" "$PSScriptRoot\downloader-installer.iss"
if ($LASTEXITCODE -ne 0) { throw 'Standalone downloader installer compilation failed' }
$DownloaderHash = (Get-FileHash -LiteralPath $DownloaderInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
"$DownloaderHash  TwinFile-Downloader-$Version-Windows-x64-Setup.exe" | Set-Content -LiteralPath "$DownloaderInstaller.sha256" -Encoding ascii
Write-Output "Standalone downloader installer built, not executed: $DownloaderInstaller"
$Installer = Join-Path $PSScriptRoot 'releases\TwinFile-3.1-Cpp-Setup.exe'
$Hash = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$Hash  TwinFile-3.1-Cpp-Setup.exe" | Set-Content -LiteralPath "$Installer.sha256" -Encoding ascii
Write-Output "Native installer built, not executed: $Installer"
