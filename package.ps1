param(
    [string]$Compiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    [string]$QtRoot,
    [string]$CompilerRoot,
    [string]$CMake = 'cmake',
    [string]$ToolRoot = (Join-Path $PSScriptRoot 'tools'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
if (-not $SkipBuild) {
    if (-not $QtRoot -or -not $CompilerRoot) { throw 'Pass -QtRoot and -CompilerRoot, or deploy first and use -SkipBuild' }
    & "$PSScriptRoot\build.ps1" -QtRoot $QtRoot -CompilerRoot $CompilerRoot -CMake $CMake -ToolRoot $ToolRoot -Package
}
if (-not (Test-Path -LiteralPath "$PSScriptRoot\dist\TwinFile\TwinFile.exe")) { throw 'Build the native portable package first' }
if (-not (Test-Path -LiteralPath $Compiler)) { throw 'Pass -Compiler with the full Inno Setup compiler path' }
& $Compiler "$PSScriptRoot\installer.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
$Installer = Join-Path $PSScriptRoot 'releases\TwinFile-3.1-Cpp-Setup.exe'
$Hash = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$Hash  TwinFile-3.1-Cpp-Setup.exe" | Set-Content -LiteralPath "$Installer.sha256" -Encoding ascii
Write-Output "Native installer built, not executed: $Installer"
