param([string]$BuildRoot = 'D:/TwinFile-Rust-Build', [string]$GccBin = 'D:/Qt/Tools/mingw1310_64/bin')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/toolchain.ps1" -BuildRoot $BuildRoot -GccBin $GccBin
Push-Location -LiteralPath $PSScriptRoot
try {
    cargo build --locked --release
    if ($LASTEXITCODE -ne 0) { throw "release build failed: $LASTEXITCODE" }
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
    $dir = Join-Path $PSScriptRoot "dist/TwinFile-Rust-Preview-$stamp"
    $run = Join-Path $PSScriptRoot "verification/release-$stamp"
    New-Item -ItemType Directory -Path $dir, $run | Out-Null
    $binary = Join-Path $env:CARGO_TARGET_DIR 'release/twinfile-rust.exe'
    $exe = Join-Path $dir TwinFileRust.exe
    Copy-Item -LiteralPath $binary -Destination $exe
    Copy-Item -LiteralPath $binary -Destination (Join-Path $dir TwinFileRustDownloader.exe)
    Copy-Item -LiteralPath (Join-Path $env:CARGO_TARGET_DIR 'release/twinfile-rust-download-host.exe') -Destination (Join-Path $dir TwinFileRustDownloadHost.exe)
    Copy-Item -LiteralPath browser-extension -Destination (Join-Path $dir 'browser-bridge') -Recurse
    Copy-Item -LiteralPath README.md,PARITY.md,LICENSE,MIUIX-NOTICE.md -Destination $dir
    if (Test-Path -LiteralPath Apache-2.0.txt) { Copy-Item -LiteralPath Apache-2.0.txt -Destination $dir }
    $manifest = cargo metadata --locked --format-version 1 --filter-platform x86_64-pc-windows-gnu | ConvertFrom-Json -AsHashtable
    if ($LASTEXITCODE -ne 0) { throw 'Unable to inventory dependencies' }
    $third = Join-Path $dir 'dependency-licenses'
    New-Item -ItemType Directory -Path $third | Out-Null
    $pkgs = $manifest.packages | Where-Object { $_.source } | Sort-Object name, version
    $pkgs | ForEach-Object { "$($_.name) $($_.version) $($_.license) $($_.repository)" } | Set-Content -LiteralPath "$dir/DEPENDENCIES.txt" -Encoding utf8
    foreach ($pkg in $pkgs) {
        $src = Split-Path -Parent $pkg.manifest_path
        $files = Get-ChildItem -LiteralPath $src -File | Where-Object Name -Match '^(licen[cs]e|copying|copyright|notice)'
        $extra = if ($pkg.license_file) { Get-Item -LiteralPath (Join-Path $src $pkg.license_file) } else { @() }
        $notices = @($files) + @($extra)
        if ($notices.Count) {
            $lic = Join-Path $third "$($pkg.name)-$($pkg.version)"
            New-Item -ItemType Directory -Path $lic | Out-Null
            foreach ($f in $notices | Sort-Object FullName -Unique) { Copy-Item -LiteralPath $f.FullName -Destination $lic }
        }
    }
    foreach ($marker in @('ready.txt','reopened.txt')) {
        $proc = Start-Process -FilePath $exe -ArgumentList @('--smoke', ('"' + $run + '"')) -WorkingDirectory $dir -WindowStyle Hidden -PassThru
        if (!$proc.WaitForExit(20000)) { Stop-Process -Id $proc.Id; throw "GUI $marker smoke test timed out" }
        if ($proc.ExitCode -ne 0 -or !(Test-Path -LiteralPath (Join-Path $run $marker))) {
            throw "GUI $marker smoke failed, exit=$($proc.ExitCode)"
        }
        "$marker exit=$($proc.ExitCode)" | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    }
    $theme = Join-Path $run 'themes'
    New-Item -ItemType Directory -Path $theme,"$theme/fixture/left/下载资料","$theme/fixture/right/示例照片" | Out-Null
    'Visual verification fixture' | Set-Content -LiteralPath "$theme/fixture/left/示例说明.txt","$theme/fixture/right/notes.txt"
    $proc = Start-Process -FilePath $exe -ArgumentList @('--theme-smoke', ('"' + $theme + '"')) -WorkingDirectory $dir -WindowStyle Hidden -PassThru
    if (!$proc.WaitForExit(90000)) { throw "Theme matrix timed out: $((Get-ChildItem -LiteralPath $theme -Filter '*.png').Count) frames" }
    if ($proc.ExitCode -ne 0 -or !(Test-Path -LiteralPath "$theme/themes-ready.txt")) {
        $reason = if (Test-Path -LiteralPath "$theme/themes-error.txt") { Get-Content -Raw -LiteralPath "$theme/themes-error.txt" }
        throw "Theme matrix failed, exit=$($proc.ExitCode) $reason"
    }
    $pngs = @(Get-ChildItem -LiteralPath $theme -Filter '*.png')
    function Get-PngWidth($png) {
        $head = [byte[]]@(Get-Content -LiteralPath $png.FullName -AsByteStream -TotalCount 24)
        [Array]::Reverse($head,16,4)
        [BitConverter]::ToUInt32($head,16)
    }
    $wide = @($pngs | Where-Object BaseName -like '*-wide' | ForEach-Object { Get-PngWidth $_ })
    $narrow = @($pngs | Where-Object BaseName -like '*-narrow' | ForEach-Object { Get-PngWidth $_ })
    if ($wide.Count -ne 8 -or $narrow.Count -ne 8 -or ($wide | Measure-Object -Minimum).Minimum -le ($narrow | Measure-Object -Maximum).Maximum) {
        throw "Theme matrix invalid sizes: wide=$wide; narrow=$narrow"
    }
    "Theme matrix: 8 wide ($($wide[0]) px), 8 narrow ($($narrow[0]) px); real nonblank captures." | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    $motionDir = Join-Path $theme motion
    $motionMarkers = @('01-before','02-transition','03-settled','04-reduced')
    if (!(Test-Path -LiteralPath "$theme/motion-ready.txt")) {
        $reason = if (Test-Path -LiteralPath "$theme/motion-error.txt") { Get-Content -Raw -LiteralPath "$theme/motion-error.txt" }
        throw "Motion smoke failed: $reason"
    }
    foreach ($name in $motionMarkers) {
        if (!(Test-Path -LiteralPath "$motionDir/$name.png")) { throw "Missing motion capture: $name" }
    }
    $frames = foreach ($name in $motionMarkers) { (Get-FileHash -LiteralPath "$motionDir/$name.png").Hash }
    if ($frames[1] -eq $frames[2]) { throw 'Intermediate and settled motion captures were identical.' }
    'Miuix motion: four actual frames; intermediate differs from settled.' | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    if (!(Test-Path -LiteralPath "$theme/menu-ready.txt")) {
        $reason = if (Test-Path -LiteralPath "$theme/menu-error.txt") { Get-Content -Raw -LiteralPath "$theme/menu-error.txt" }
        throw "Menu smoke failed: $reason"
    }
    if (!(Test-Path -LiteralPath "$theme/switch-ready.txt")) {
        $reason = if (Test-Path -LiteralPath "$theme/switch-error.txt") { Get-Content -Raw -LiteralPath "$theme/switch-error.txt" }
        throw "Pane switching failed: $reason"
    }
    $switched = foreach ($name in 'pane-left','pane-right') {
        (Get-FileHash -LiteralPath "$theme/switch/$name.png").Hash
    }
    if (@($switched | Select-Object -Unique).Count -ne 2) { throw 'Pane activation captures repeat.' }
    Get-Content -LiteralPath "$theme/switch-ready.txt" | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    if (!(Test-Path -LiteralPath "$theme/overlay-ready.txt")) {
        $reason = if (Test-Path -LiteralPath "$theme/overlay-error.txt") { Get-Content -Raw -LiteralPath "$theme/overlay-error.txt" }
        throw "Miuix overlay smoke failed: $reason"
    }
    foreach ($name in 'search-light','results-light') {
        if (!(Test-Path -LiteralPath "$theme/overlay/$name.png")) { throw "Missing overlay capture: $name" }
    }
    Get-Content -LiteralPath "$theme/overlay-ready.txt" | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    $menuFrames = foreach ($name in '01-main','02-settings','03-sort') {
        if (!(Test-Path -LiteralPath "$theme/menu/$name.png")) { throw "Missing menu capture: $name" }
        (Get-FileHash -LiteralPath "$theme/menu/$name.png").Hash
    }
    if (@($menuFrames | Select-Object -Unique).Count -ne 3) { throw 'Menu captures repeat.' }
    Get-Content -LiteralPath "$theme/menu-ready.txt" | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    try {
        $bridgeResult = node ./tests/native_bridge.cjs $dir $run
        if ($LASTEXITCODE) { throw "Native bridge test failed: $LASTEXITCODE" }
        $bridgeResult | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    } finally {
        $verifiedChild = Join-Path $dir 'TwinFileRustDownloader.exe'
        Get-Process -Name TwinFileRustDownloader -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $verifiedChild -and $_.MainWindowHandle } |
            ForEach-Object { [void]$_.CloseMainWindow() }
    }
    $zip = "$dir.zip"
    Compress-Archive -LiteralPath $dir -DestinationPath $zip
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $required = @('TwinFileRust.exe','TwinFileRustDownloader.exe','TwinFileRustDownloadHost.exe','PARITY.md')
        $required += Get-ChildItem -LiteralPath (Join-Path $dir 'browser-bridge') -File | ForEach-Object { "browser-bridge/$($_.Name)" }
        $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\','/') })
        foreach ($name in $required) {
            if ("TwinFile-Rust-Preview-$stamp/$name" -notin $entries) { throw "Incomplete ZIP: $name" }
        }
    } finally { $archive.Dispose() }
    Get-FileHash -LiteralPath $exe,(Join-Path $dir 'TwinFileRustDownloadHost.exe'),$zip | Format-List | Out-String | Add-Content -LiteralPath "$run/VERIFICATION.txt"
    Get-Content -LiteralPath "$run/VERIFICATION.txt"
    "Portable folder: $dir"
    "Preview ZIP: $zip"
} finally { Pop-Location }
