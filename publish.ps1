param(
    [string]$QtRoot,
    [string]$CompilerRoot,
    [string]$ToolRoot = (Join-Path $PSScriptRoot 'tools'),
    [string]$CMake = 'cmake',
    [string]$Compiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    [string]$Proxy = $env:HTTPS_PROXY,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 or later' }
$Repository = '350624/TwinFile'
function Invoke-Git {
    param([string[]]$Arguments)
    $Output = & git -C $PSScriptRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $($Arguments[0])" }
    return $Output
}
$VersionMatch = [regex]::Match((Get-Content -LiteralPath "$PSScriptRoot\CMakeLists.txt" -Raw), 'project\(TwinFileNative VERSION (\d+\.\d+\.\d+)')
if (-not $VersionMatch.Success) { throw 'Cannot read project version' }
$Version = $VersionMatch.Groups[1].Value
$Tag = "v$Version"
$InstallerDefinition = Get-Content -LiteralPath "$PSScriptRoot\installer.iss" -Raw
if ($InstallerDefinition -notmatch ('(?m)^#define MyAppVersion "' + [regex]::Escape($Version) + '"') -or
    $InstallerDefinition -notmatch ('(?m)^VersionInfoVersion=' + [regex]::Escape($Version) + '\.0\s*$')) { throw 'Installer and project versions differ' }
$Changelog = Get-Content -LiteralPath "$PSScriptRoot\CHANGELOG.md" -Raw
$Section = [regex]::Match($Changelog, '(?ms)^## \[' + [regex]::Escape($Version) + '\] - \d{4}-\d{2}-\d{2}\s*\r?\n(.*?)(?=^## |^\[\d|\z)')
if (-not $Section.Success -or $Section.Groups[1].Value.Trim().Length -lt 40) { throw 'Write a substantive changelog section for this version first' }
if ((Invoke-Git @('branch','--show-current')) -ne 'main') { throw 'Publish from main only' }
if (Invoke-Git @('status','--porcelain','--untracked-files=all')) { throw 'Commit intended changes and leave a clean worktree before publishing' }
foreach ($RemoteKind in @('get-url','get-url --push')) {
    $RemoteArguments = @('remote') + $RemoteKind.Split(' ') + @('origin')
    $Remote = Invoke-Git $RemoteArguments
    if ($Remote -notmatch '^https://github\.com/350624/TwinFile(?:\.git)?/?$|^git@github\.com:350624/TwinFile(?:\.git)?$') { throw 'Unexpected origin; refusing to publish another repository' }
}
$Commit = Invoke-Git @('rev-parse','HEAD')
if ($CheckOnly) { Write-Output "Ready for build and publication checks: $Tag at $Commit (no network or upload performed)"; return }
if (-not $QtRoot -or -not $CompilerRoot) { throw 'Pass -QtRoot and -CompilerRoot' }
$Token = $env:GH_TOKEN
if (-not $Token) { $Token = $env:GITHUB_TOKEN }
if (-not $Token) {
    $PreviousPrompt = $env:GIT_TERMINAL_PROMPT
    $PreviousInteractive = $env:GCM_INTERACTIVE
    try {
        $env:GIT_TERMINAL_PROMPT = '0'
        $env:GCM_INTERACTIVE = 'Never'
        $Credential = @('protocol=https','host=github.com','') | & git credential fill 2>$null
        if ($LASTEXITCODE -ne 0) { throw 'Log in to GitHub with Git Credential Manager first' }
        $Secret = $Credential | Where-Object { $_.StartsWith('password=') } | Select-Object -First 1
        if ($Secret) { $Token = $Secret.Substring(9) }
    } finally {
        $env:GIT_TERMINAL_PROMPT = $PreviousPrompt
        $env:GCM_INTERACTIVE = $PreviousInteractive
        $Credential = $null
        $Secret = $null
    }
}
if (-not $Token) { throw 'GitHub authentication unavailable' }
$Headers = @{ Authorization="Bearer $Token"; Accept='application/vnd.github+json'; 'User-Agent'='TwinFile-Publisher'; 'X-GitHub-Api-Version'='2022-11-28' }
$Token = $null
function Invoke-Api {
    param([string]$Path, [string]$Method='Get', [object]$Body=$null)
    $Parameters = @{ Uri="https://api.github.com/repos/$Repository/$Path"; Method=$Method; Headers=$Headers; TimeoutSec=120 }
    if ($null -ne $Body) { $Parameters.Body = ConvertTo-Json -InputObject $Body -Depth 12 -Compress; $Parameters.ContentType = 'application/json; charset=utf-8' }
    if ($Proxy) { $Parameters.Proxy = $Proxy }
    Invoke-RestMethod @Parameters
}
try {
    $Existing = $null
    try { $Existing = Invoke-Api "releases/tags/$Tag" } catch {
        if (-not $_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw }
    }
    $LocalTag = Invoke-Git @('tag','--list',$Tag)
    if ($LocalTag -and (Invoke-Git @('rev-list','-n','1',$Tag)) -ne $Commit) { throw 'Version tag points to another commit; increment the version' }
    $RemoteTag = Invoke-Git @('ls-remote','--tags','origin',"refs/tags/$Tag","refs/tags/$Tag^{}")
    if ($RemoteTag) {
        $TagLines = @($RemoteTag)
        $TargetLine = $TagLines | Where-Object { $_.EndsWith('^{}') } | Select-Object -First 1
        if (-not $TargetLine) { $TargetLine = $TagLines[0] }
        if (($TargetLine -split '\s+')[0] -ne $Commit) { throw 'Remote tag points to another commit; never overwrite a published version' }
    }
    if ($Existing -and -not $Existing.draft) {
        $Expected = @("TwinFile-$Version-Windows-x64-Setup.exe", "TwinFile-$Version-source.zip", 'SHA256SUMS.txt', 'release-components.json')
        if (-not $RemoteTag -or @($Expected | Where-Object { $_ -notin $Existing.assets.name }).Count) { throw 'Published release is incomplete; inspect it manually, do not overwrite it' }
        Write-Output "Already published: $($Existing.html_url)"
        return
    }
    & "$PSScriptRoot\tests\release_tests.ps1"
    & "$PSScriptRoot\build.ps1" -QtRoot $QtRoot -CompilerRoot $CompilerRoot -ToolRoot $ToolRoot -CMake $CMake -Package
    & "$PSScriptRoot\verify-ui.ps1" -QtRoot $QtRoot
    & "$PSScriptRoot\verify.ps1"
    & "$PSScriptRoot\package.ps1" -SkipBuild -Compiler $Compiler
    if (Invoke-Git @('status','--porcelain','--untracked-files=all')) { throw 'Tracked source changed during build; review and commit before release' }
    $ReleaseDirectory = Join-Path $PSScriptRoot 'releases'
    $Setup = Join-Path $ReleaseDirectory "TwinFile-$Version-Windows-x64-Setup.exe"
    Copy-Item -LiteralPath "$ReleaseDirectory\TwinFile-3.1-Cpp-Setup.exe" -Destination $Setup -Force
    $Source = Join-Path $ReleaseDirectory "TwinFile-$Version-source.zip"
    Invoke-Git @('archive','--format=zip',"--prefix=TwinFile-$Version/","--output=$Source",$Commit)
    $Components = Join-Path $ReleaseDirectory 'release-components.json'
    Copy-Item -LiteralPath "$PSScriptRoot\dist\TwinFile\release-components.json" -Destination $Components -Force
    $Artifacts = @($Setup,$Source,$Components)
    $Sums = Join-Path $ReleaseDirectory 'SHA256SUMS.txt'
    $Lines = foreach ($Artifact in $Artifacts) { "$((Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($Artifact))" }
    [IO.File]::WriteAllLines($Sums, $Lines, [Text.UTF8Encoding]::new($false))
    $Artifacts += $Sums
    Invoke-Git @('push','origin','main')
    if (-not $LocalTag) { Invoke-Git @('tag','-a',$Tag,$Commit,'-m',"TwinFile $Version") }
    Invoke-Git @('push','origin',"refs/tags/$Tag")
    $Notes = $Section.Groups[1].Value.Trim() + "`n`n安装：下载 ``TwinFile-$Version-Windows-x64-Setup.exe``，校验 SHA256 后运行。源码 ZIP 不能直接运行。`n`n源码提交：$Commit`n`n完整说明：https://github.com/$Repository/blob/$Tag/docs/INSTALL.md"
    if (-not $Existing) { $Existing = Invoke-Api 'releases' 'Post' @{ tag_name=$Tag; target_commitish=$Commit; name="TwinFile $Version · Windows x64"; body=$Notes; draft=$true; prerelease=$false } }
    else { $Existing = Invoke-Api "releases/$($Existing.id)" 'Patch' @{ body=$Notes; name="TwinFile $Version · Windows x64" } }
    foreach ($Artifact in $Artifacts) {
        $Name = [IO.Path]::GetFileName($Artifact)
        $Hash = (Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash.ToLowerInvariant()
        $OldAsset = $Existing.assets | Where-Object name -eq $Name | Select-Object -First 1
        if ($OldAsset) {
            if ($OldAsset.digest -eq "sha256:$Hash" -and $OldAsset.size -eq (Get-Item -LiteralPath $Artifact).Length) { continue }
            $null = Invoke-Api "releases/assets/$($OldAsset.id)" 'Delete'
        }
        Write-Output "Uploading $Name"
        $UploadParameters = @{ Uri="https://uploads.github.com/repos/$Repository/releases/$($Existing.id)/assets?name=$([Uri]::EscapeDataString($Name))"; Method='Post'; Headers=$Headers; InFile=$Artifact; ContentType='application/octet-stream'; TimeoutSec=1800 }
        if ($Proxy) { $UploadParameters.Proxy = $Proxy }
        $Uploaded = Invoke-RestMethod @UploadParameters
        if ($Uploaded.size -ne (Get-Item -LiteralPath $Artifact).Length -or $Uploaded.digest -ne "sha256:$Hash") { throw "Uploaded asset verification failed: $Name; draft retained" }
    }
    $Published = Invoke-Api "releases/$($Existing.id)" 'Patch' @{ draft=$false; make_latest='true' }
    if ($Published.draft -or @($Artifacts | Where-Object { [IO.Path]::GetFileName($_) -notin $Published.assets.name }).Count) { throw 'Release publication verification failed' }
    Write-Output "Published: $($Published.html_url)"
} finally {
    $Headers.Clear()
}
