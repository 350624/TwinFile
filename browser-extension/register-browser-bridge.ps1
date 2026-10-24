param(
    [Parameter(Mandatory=$true)][ValidateSet('Chrome','Edge','Both')][string]$Browser,
    [string]$ChromeExtensionId,
    [string]$EdgeExtensionId,
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\TwinFile Downloader')
)
$ErrorActionPreference = 'Stop'
$Browsers = switch ($Browser) { 'Chrome' { @(@{ Name='Google\Chrome'; File='chrome'; Id=$ChromeExtensionId }) } 'Edge' { @(@{ Name='Microsoft\Edge'; File='edge'; Id=$EdgeExtensionId }) } 'Both' { @(@{ Name='Google\Chrome'; File='chrome'; Id=$ChromeExtensionId },@{ Name='Microsoft\Edge'; File='edge'; Id=$EdgeExtensionId }) } }
foreach ($Entry in $Browsers) { if ($Entry.Id -notmatch '^[a-p]{32}$') { throw "A valid extension ID is required for $($Entry.File)." } }
$Executable = Join-Path $InstallDirectory 'TwinFileDownloader.exe'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Standalone downloader not found: $Executable" }
$HostExecutable = Join-Path $InstallDirectory 'TwinFileDownloadHost.exe'
if (-not (Test-Path -LiteralPath $HostExecutable -PathType Leaf)) { throw "Browser messaging host not found: $HostExecutable" }
$HostDirectory = Join-Path $env:LOCALAPPDATA 'TwinFile Downloader\browser-bridge'
New-Item -ItemType Directory -Path $HostDirectory -Force | Out-Null
function Register-BrowserHost([string]$RegistryBrowser, [string]$ManifestName, [string]$ExtensionId) {
    $Manifest = [ordered]@{
        name = 'com.twinfile.downloader'
        description = 'TwinFile Downloader browser bridge'
        path = $HostExecutable
        type = 'stdio'
        allowed_origins = @("chrome-extension://$ExtensionId/")
    } | ConvertTo-Json -Depth 4
    $ManifestPath = Join-Path $HostDirectory "$ManifestName-com.twinfile.downloader.json"
    [IO.File]::WriteAllText($ManifestPath, $Manifest, [Text.UTF8Encoding]::new($false))
    $RegistryPath = "HKCU:\Software\$RegistryBrowser\NativeMessagingHosts\com.twinfile.downloader"
    New-Item -Path $RegistryPath -Force | Out-Null
    Set-Item -Path $RegistryPath -Value $ManifestPath
}
foreach ($Entry in $Browsers) { Register-BrowserHost $Entry.Name $Entry.File $Entry.Id }
Write-Output "Restart $Browser to activate the browser bridge."
