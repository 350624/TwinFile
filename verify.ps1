param([string]$Destination = (Join-Path $PSScriptRoot 'dist\TwinFile'))
$ErrorActionPreference = 'Stop'
$Destination = (Resolve-Path -LiteralPath $Destination).Path
$Executable = Join-Path $Destination 'TwinFile.exe'
if (-not (Test-Path -LiteralPath $Executable)) { throw 'Build the portable native app first' }
$Evidence = Join-Path $PSScriptRoot 'verification'
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$SourceForbidden = @(Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File | Where-Object { $_.Name -match '(?i)(\.py[co]?$|^python.*\.(dll|exe)$|pyside|shiboken)' })
if ($SourceForbidden.Count) { throw "Python artifacts found in native project: $($SourceForbidden.FullName -join ', ')" }
$SourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src'),(Join-Path $PSScriptRoot 'sdk'),(Join-Path $PSScriptRoot 'tests'),(Join-Path $PSScriptRoot 'assets'),(Join-Path $PSScriptRoot 'vendor') -Recurse -File)
$SourceFiles += @(Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object { $_.Extension -in @('.ps1','.iss') -or $_.Name -eq 'CMakeLists.txt' })
$Manifest = foreach ($SourceFile in ($SourceFiles | Sort-Object FullName)) { "$((Get-FileHash -LiteralPath $SourceFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetRelativePath($PSScriptRoot, $SourceFile.FullName).Replace('\','/'))" }
$Manifest | Set-Content -LiteralPath (Join-Path $Evidence 'native-source-manifest.sha256') -Encoding utf8
$Screenshot = Join-Path $Evidence 'native-window.png'
$Forbidden = @(Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object { $_.Name -match '(?i)(\.py[co]?$|^python.*\.(dll|exe)$|pyside|shiboken)' })
if ($Forbidden.Count) { throw "Python artifacts detected: $($Forbidden.FullName -join ', ')" }
$PreviousEnvironment = @{}
foreach ($Name in @('Path','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QPA_PLATFORM')) { $PreviousEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name) }
try {
$env:Path = "$Destination;$env:SystemRoot\System32;$env:SystemRoot"
$env:QT_PLUGIN_PATH = $Destination
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Destination 'platforms'
$env:QT_QPA_PLATFORM = 'windows'
$Started = [DateTime]::UtcNow
$Process = Start-Process -FilePath $Executable -ArgumentList @('--smoke-test','--screenshot',('"' + $Screenshot + '"')) -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 350
$LoadedModules = @()
if (-not $Process.HasExited) { $LoadedModules = @($Process.Modules | ForEach-Object { $_.ModuleName }) }
if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Native smoke test timed out' }
if ($Process.ExitCode -ne 0) { throw "Native smoke test failed: $($Process.ExitCode)" }
if ($LoadedModules | Where-Object { $_ -match '(?i)(python|pyside|shiboken)' }) { throw 'Python runtime loaded into native process' }
if (-not ($LoadedModules -contains 'Qt6Widgets.dll')) { throw 'Failed to sample native Qt process modules' }
if ((Get-Item -LiteralPath $Screenshot).LastWriteTimeUtc -lt $Started.AddSeconds(-1)) { throw 'Screenshot was not produced by this run' }
$Bytes = [IO.File]::ReadAllBytes($Screenshot)
if ($Bytes.Length -lt 24 -or [Text.Encoding]::ASCII.GetString($Bytes,1,3) -ne 'PNG') { throw 'Screenshot is not PNG' }
$Width = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($Bytes,16))
$Height = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($Bytes,20))
if ($Width -lt 800 -or $Height -lt 500) { throw 'Native window screenshot is too small' }
$Report = [ordered]@{
    checkedAt = [DateTime]::UtcNow.ToString('o')
    executable = $Executable
    sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    packageBytes = (Get-ChildItem -LiteralPath $Destination -Recurse -File | Measure-Object Length -Sum).Sum
    sourceLanguage = 'C++20; native C compression dependency'
    pythonArtifacts = $Forbidden.Count
    sourcePythonArtifacts = $SourceForbidden.Count
    sourceManifest = Join-Path $Evidence 'native-source-manifest.sha256'
    loadedModules = $LoadedModules
    smokeExitCode = $Process.ExitCode
    screenshot = $Screenshot
    screenshotWidth = $Width
    screenshotHeight = $Height
    realPhoneTested = $false
    qqFileSent = $false
    installerExecuted = $false
    completeLegacyParity = $false
}
$Report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Evidence 'native-verification.json') -Encoding utf8
Write-Output "Native portable smoke test passed: $Screenshot"
Write-Output "Python files/runtime: $($Forbidden.Count)"
} finally {
    foreach ($Name in $PreviousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($Name, $PreviousEnvironment[$Name]) }
}
