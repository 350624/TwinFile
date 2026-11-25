[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][ValidateSet('Chrome','Edge')][string]$Browser,
    [ValidatePattern('^[a-p]{32}$')][string]$ExtensionId,
    [switch]$Unregister
)
$ErrorActionPreference='Stop'
$name='com.twinfile.rust.downloader'
$vendor=if($Browser -eq 'Chrome'){'Google\Chrome'}else{'Microsoft\Edge'}
$key="HKCU:\Software\$vendor\NativeMessagingHosts\$name"
$manifest=Join-Path $PSScriptRoot "$name-$Browser.json"
$hostExe=Join-Path (Split-Path -Parent $PSScriptRoot) 'TwinFileRustDownloadHost.exe'
if ($Unregister) {
    if (Test-Path -LiteralPath $key) {
        $registered=(Get-Item -LiteralPath $key).GetValue('')
        if ($registered -ne $manifest) {throw 'Path belongs to another TwinFile Rust package; use its registration script.'}
        if ($PSCmdlet.ShouldProcess($key,'Remove Native Messaging host registration')){Remove-Item -LiteralPath $key}
    }
    if (Test-Path -LiteralPath $manifest) {
        if ($PSCmdlet.ShouldProcess($manifest,'Remove generated manifest')){Remove-Item -LiteralPath $manifest}
    }
    return
}
if(!$ExtensionId){throw 'Specify -ExtensionId using the ID shown on the browser extensions page.'}
if(!(Test-Path -LiteralPath $hostExe -PathType Leaf)){throw 'Use the browser-bridge directory next to the packaged Rust host executable.'}
if(!$PSCmdlet.ShouldProcess($key,'Register TwinFile Rust for this browser')){return}
$data=[ordered]@{
    name=$name
    description='TwinFile Rust Downloader bridge'
    path=(Get-Item -LiteralPath $hostExe).FullName
    type='stdio'
    allowed_origins=@("chrome-extension://$ExtensionId/")
}
$data | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
if (!(Test-Path -LiteralPath $key)) {New-Item -Path $key -Force | Out-Null}
(Get-Item -LiteralPath $key).SetValue('', $manifest, [Microsoft.Win32.RegistryValueKind]::String)
