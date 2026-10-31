param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$CompilerRoot,
    [string]$CMake = 'cmake',
    [string]$ToolRoot = (Join-Path $PSScriptRoot 'tools'),
    [switch]$SkipTests,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$VersionMatch = [regex]::Match((Get-Content -LiteralPath "$Root\CMakeLists.txt" -Raw), 'project\(TwinFileNative VERSION (\d+\.\d+\.\d+)')
if (-not $VersionMatch.Success) { throw 'Cannot read project version' }
$Version = $VersionMatch.Groups[1].Value
$Node = @(Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Where-Object { $_.Source -is [string] } | Select-Object -First 1)
if (-not $Node.Count) { throw 'Node.js is required for browser bridge tests' }
& $Node[0].Path "$Root\tests\browser_bridge_tests.cjs"
if ($LASTEXITCODE -ne 0) { throw 'Browser bridge regression tests failed' }
$Build = Join-Path $Root 'build'
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$CompilerRoot = (Resolve-Path -LiteralPath $CompilerRoot).Path
$ToolRoot = [IO.Path]::GetFullPath($ToolRoot)
$CMakeCommand = Get-Command $CMake -CommandType Application -ErrorAction Stop
$CMake = $CMakeCommand.Source
if (-not $SkipTests -or $Package) {
    foreach ($Dependency in (Get-Content -LiteralPath "$Root\tools\slides-dependencies.json" -Raw | ConvertFrom-Json)) {
        $SlideFile = Join-Path $ToolRoot "slides/$($Dependency.artifact)-$($Dependency.version).jar"
        if (-not (Test-Path -LiteralPath $SlideFile) -or (Get-FileHash -LiteralPath $SlideFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Dependency.sha256) { throw "Missing or changed slide dependency: $SlideFile. Run tools/prepare-slides.ps1 first." }
    }
    foreach ($Relative in @('adb/adb.exe','adb/AdbWinApi.dll','adb/AdbWinUsbApi.dll','adb/fastboot.exe','apktool.jar','ecj.jar','apksigner.jar','runtime/bin/java.exe','runtime/bin/keytool.exe','hfx/HybridFileXfer-phone.apk')) {
        if (-not (Test-Path -LiteralPath (Join-Path $ToolRoot $Relative) -PathType Leaf)) {
            throw "Missing external tool: $Relative. See tools/README.md; use -SkipTests for a desktop-only build without deployment."
        }
    }
}
$PreviousPath = $env:Path
try {
$env:Path = "$CompilerRoot\bin;$QtRoot\bin;$env:SystemRoot\System32;$env:SystemRoot"
foreach ($required in @($CMake, "$CompilerRoot\bin\g++.exe", "$QtRoot\bin\windeployqt.exe")) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing native build tool: $required" }
}
& $CMake -S $Root -B $Build -G 'MinGW Makefiles' "-DCMAKE_BUILD_TYPE=Release" "-DBUILD_TESTING=ON" "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_CXX_COMPILER=$CompilerRoot/bin/g++.exe" "-DCMAKE_MAKE_PROGRAM=$CompilerRoot/bin/mingw32-make.exe" "-DTWINFILE_TOOL_ROOT=$ToolRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $CMake --build $Build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native C++ compilation failed' }
if (-not $SkipTests) {
$TestResults = Join-Path $Build 'native-test-results.txt'
& "$Build\bin\twinfile_tests.exe" -platform offscreen -o "$TestResults,txt"
$TestExitCode = $LASTEXITCODE
Get-Content -LiteralPath $TestResults
if ($TestExitCode -ne 0) { throw 'Native regression tests failed' }
$MigrationResults = Join-Path $Build 'migration-test-results.txt'
& "$Build\bin\twinfile_migration_tests.exe" -platform offscreen -o "$MigrationResults,txt"
$MigrationExitCode = $LASTEXITCODE
Get-Content -LiteralPath $MigrationResults
if ($MigrationExitCode -ne 0) { throw 'Migration regression tests failed' }
$ExtensionResults = Join-Path $Build 'extension-test-results.txt'
& "$Build\bin\twinfile_extension_tests.exe" -platform offscreen -o "$ExtensionResults,txt"
$ExtensionExitCode = $LASTEXITCODE
Get-Content -LiteralPath $ExtensionResults
if ($ExtensionExitCode -ne 0) { throw 'AI and extension regression tests failed' }
$AgentResults = Join-Path $Build 'agent-test-results.txt'
& "$Build\bin\twinfile_agent_tests.exe" -platform offscreen -o "$AgentResults,txt"
$AgentExitCode = $LASTEXITCODE
Get-Content -LiteralPath $AgentResults
if ($AgentExitCode -ne 0) { throw 'Built-in Agent regression tests failed' }
$McpResults = Join-Path $Build 'mcp-test-results.txt'
& "$Build\bin\twinfile_mcp_tests.exe" -platform offscreen -o "$McpResults,txt"
$McpExitCode = $LASTEXITCODE
Get-Content -LiteralPath $McpResults
if ($McpExitCode -ne 0) { throw 'MCP regression tests failed' }
$DownloadResults = Join-Path $Build 'download-test-results.txt'
& "$Build\bin\twinfile_download_tests.exe" -platform offscreen -o "$DownloadResults,txt"
$DownloadExitCode = $LASTEXITCODE
Get-Content -LiteralPath $DownloadResults
if ($DownloadExitCode -ne 0) { throw 'Direct download regression tests failed' }
New-Item -ItemType Directory -Path "$Root\verification\test-results" -Force | Out-Null
Copy-Item -LiteralPath $TestResults,$MigrationResults,$ExtensionResults,$AgentResults,$McpResults,$DownloadResults -Destination "$Root\verification\test-results" -Force
} else {
    Write-Output 'Test execution skipped; this is not a passing test result.'
}
if ($Package) {
    $Destination = Join-Path $Root 'dist\TwinFile'
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -LiteralPath "$Build\bin\TwinFile.exe" -Destination $Destination -Force
    & "$QtRoot\bin\windeployqt.exe" --release --compiler-runtime --no-translations "$Destination\TwinFile.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
    $DownloaderDestination = Join-Path $Root "dist\TwinFileDownloader-$Version"
    New-Item -ItemType Directory -Path $DownloaderDestination -Force | Out-Null
    foreach ($Name in @('TwinFileDownloader.exe','TwinFileDownloadHost.exe')) {
        Copy-Item -LiteralPath "$Build\bin\$Name" -Destination $DownloaderDestination -Force
        & "$QtRoot\bin\windeployqt.exe" --release --compiler-runtime --no-translations "$DownloaderDestination\$Name"
        if ($LASTEXITCODE -ne 0) { throw "Qt deployment failed: $Name" }
    }
    $Bridge = Join-Path $DownloaderDestination 'browser-bridge'
    New-Item -ItemType Directory -Path $Bridge -Force | Out-Null
    Copy-Item -LiteralPath "$Root\browser-extension\manifest.json","$Root\browser-extension\background.js","$Root\browser-extension\popup.html","$Root\browser-extension\popup.js","$Root\browser-extension\register-browser-bridge.ps1","$Root\browser-extension\README.md" -Destination $Bridge -Force
    if ($env:OS -eq 'Windows_NT') { $null = Get-Command Compress-Archive -ErrorAction Stop; Compress-Archive -Path "$Root\browser-extension\manifest.json","$Root\browser-extension\background.js","$Root\browser-extension\popup.html","$Root\browser-extension\popup.js" -DestinationPath "$DownloaderDestination\browser-extension.zip" -Force }
    $ToolSource = $ToolRoot
    $ToolDestination = Join-Path $Destination 'tools'
    New-Item -ItemType Directory -Path "$ToolDestination\adb" -Force | Out-Null
    Copy-Item -LiteralPath "$Root\tools\README.md" -Destination $ToolDestination -Force
    foreach ($Name in @('adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll','fastboot.exe')) {
        $SourceFile = Join-Path "$ToolSource\adb" $Name
        $TargetFile = Join-Path "$ToolDestination\adb" $Name
        if ((Test-Path -LiteralPath $TargetFile -PathType Leaf) -and
            (Get-FileHash -LiteralPath $SourceFile -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $TargetFile -Algorithm SHA256).Hash) { continue }
        Copy-Item -LiteralPath $SourceFile -Destination $TargetFile -Force
    }
    foreach ($Name in @('apktool.jar','apksigner.jar')) {
        Copy-Item -LiteralPath "$ToolSource\$Name" -Destination $ToolDestination -Force
    }
    New-Item -ItemType Directory -Path "$ToolDestination\slides" -Force | Out-Null
    foreach ($Dependency in (Get-Content -LiteralPath "$Root\tools\slides-dependencies.json" -Raw | ConvertFrom-Json)) {
        $SlideName = "$($Dependency.artifact)-$($Dependency.version).jar"
        Copy-Item -LiteralPath "$ToolSource\slides\$SlideName" -Destination "$ToolDestination\slides" -Force
    }
    New-Item -ItemType Directory -Path "$ToolDestination\hfx" -Force | Out-Null
    Copy-Item -LiteralPath "$ToolSource\hfx\HybridFileXfer-phone.apk" -Destination "$ToolDestination\hfx" -Force
    New-Item -ItemType Directory -Path "$ToolDestination\runtime" -Force | Out-Null
    Get-ChildItem -LiteralPath "$ToolSource\runtime" | Copy-Item -Destination "$ToolDestination\runtime" -Recurse -Force
    foreach ($Name in @('README.md','CHANGELOG.md','MIGRATION.md','THIRD_PARTY.md','VERIFICATION.md','PLUGIN_SDK.md','LICENSE','NOTICE.md','CONTRIBUTING.md','SECURITY.md','docs')) {
        if (Test-Path -LiteralPath "$Root\$Name") { Copy-Item -LiteralPath "$Root\$Name" -Destination $Destination -Recurse -Force }
    }
    New-Item -ItemType Directory -Path "$Destination\licenses" -Force | Out-Null
    New-Item -ItemType Directory -Path "$Destination\sdk","$Destination\plugin-examples" -Force | Out-Null
    Get-ChildItem -LiteralPath "$Root\sdk" | Copy-Item -Destination "$Destination\sdk" -Recurse -Force
    Copy-Item -LiteralPath "$Build\bin\twinfile_example_plugin.dll" -Destination "$Destination\plugin-examples\selection-example.dll" -Force
    Copy-Item -LiteralPath "$Root\vendor\zlib\LICENSE" -Destination "$Destination\licenses\zlib.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\zlib\minizip\LICENSE.Info-Zip" -Destination "$Destination\licenses\Info-Zip.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\bzip2\LICENSE" -Destination "$Destination\licenses\bzip2.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\xz\COPYING.0BSD" -Destination "$Destination\licenses\XZ-0BSD.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\zstd\LICENSE" -Destination "$Destination\licenses\Zstandard-BSD.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\webp\COPYING" -Destination "$Destination\licenses\libwebp-BSD.txt" -Force
    Copy-Item -LiteralPath "$Root\vendor\webp\PATENTS" -Destination "$Destination\licenses\libwebp-PATENTS.txt" -Force
    if (Test-Path -LiteralPath "$Root\licenses") { Get-ChildItem -LiteralPath "$Root\licenses" | Copy-Item -Destination "$Destination\licenses" -Recurse -Force }
    New-Item -ItemType Directory -Path "$DownloaderDestination\licenses" -Force | Out-Null
    if (Test-Path -LiteralPath "$Root\licenses") { Get-ChildItem -LiteralPath "$Root\licenses" | Copy-Item -Destination "$DownloaderDestination\licenses" -Recurse -Force }
    foreach ($Name in @('LICENSE','NOTICE.md')) { Copy-Item -LiteralPath "$Root\$Name" -Destination "$DownloaderDestination\licenses" -Force }
    New-Item -ItemType Directory -Path "$DownloaderDestination\licenses\mingw","$DownloaderDestination\licenses\qt-sbom" -Force | Out-Null
    foreach ($Name in @('gcc','mingw-w64','winpthreads')) {
        $LicenseDirectory = Join-Path $CompilerRoot "licenses\$Name"
        if (-not (Test-Path -LiteralPath $LicenseDirectory)) { throw "Missing runtime notices: $Name" }
        Copy-Item -LiteralPath $LicenseDirectory -Destination "$DownloaderDestination\licenses\mingw" -Recurse -Force
    }
    foreach ($Module in @('qtbase')) {
        $Sbom = @(Get-ChildItem -LiteralPath "$QtRoot\sbom" -Filter "$Module-*.cdx.json")
        if ($Sbom.Count -ne 1) { throw "Expected one SDK SBOM for $Module" }
        Copy-Item -LiteralPath $Sbom[0].FullName -Destination "$DownloaderDestination\licenses\qt-sbom" -Force
    }
    $DownloaderComponents = foreach ($File in (Get-ChildItem -LiteralPath $DownloaderDestination -Recurse -File | Where-Object { $_.Extension -in @('.exe','.dll') } | Sort-Object FullName)) {
        [ordered]@{ path=[IO.Path]::GetRelativePath($DownloaderDestination,$File.FullName).Replace('\','/'); size=$File.Length; sha256=(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); fileVersion=$File.VersionInfo.FileVersion }
    }
    [ordered]@{ schema=1; components=@($DownloaderComponents); sources='licenses/SOURCES.md' } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$DownloaderDestination\release-components.json" -Encoding utf8
    New-Item -ItemType Directory -Path "$Destination\licenses\mingw","$Destination\licenses\qt-sbom" -Force | Out-Null
    foreach ($Name in @('gcc','mingw-w64','winpthreads')) {
        $LicenseDirectory = Join-Path $CompilerRoot "licenses\$Name"
        if (-not (Test-Path -LiteralPath $LicenseDirectory)) { throw "Missing runtime notices: $Name" }
        Copy-Item -LiteralPath $LicenseDirectory -Destination "$Destination\licenses\mingw" -Recurse -Force
    }
    foreach ($Module in @('qtbase','qtsvg','qtmultimedia')) {
        $Sbom = @(Get-ChildItem -LiteralPath "$QtRoot\sbom" -Filter "$Module-*.cdx.json")
        if ($Sbom.Count -ne 1) { throw "Expected one SDK SBOM for $Module" }
        Copy-Item -LiteralPath $Sbom[0].FullName -Destination "$Destination\licenses\qt-sbom" -Force
    }
    $Components = foreach ($File in (Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object { $_.Extension -in @('.exe','.dll','.jar','.apk') } | Sort-Object FullName)) {
        [ordered]@{ path=[IO.Path]::GetRelativePath($Destination,$File.FullName).Replace('\','/'); size=$File.Length; sha256=(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); fileVersion=$File.VersionInfo.FileVersion }
    }
    [ordered]@{ schema=1; components=@($Components); sources='licenses/SOURCES.md' } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$Destination\release-components.json" -Encoding utf8
    $Forbidden = @(Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object { $_.Name -match '(?i)(\.py[co]?$|^python.*\.(dll|exe)$|pyside|shiboken)' })
    if ($Forbidden.Count) { throw "Python artifacts found in native package: $($Forbidden.FullName -join ', ')" }
    Write-Output "Native portable application: $Destination\TwinFile.exe"
}
} finally {
    $env:Path = $PreviousPath
}
