param([string]$BuildRoot = 'D:/TwinFile-Rust-Build', [string]$GccBin = 'D:/Qt/Tools/mingw1310_64/bin')
if ($BuildRoot -notmatch '^[\x20-\x7e]+$') { throw 'Use an ASCII build/cache path for MinGW.' }
$env:RUSTUP_HOME = Join-Path $BuildRoot 'rustup'
$env:CARGO_HOME = Join-Path $BuildRoot 'cargo'
$env:CARGO_TARGET_DIR = Join-Path $BuildRoot 'target-v2'
if (-not (Test-Path -LiteralPath "$env:CARGO_HOME/bin/cargo.exe")) { throw 'Install the x86_64-pc-windows-gnu toolchain in the dedicated ASCII build/cache directory.' }
if (-not (Test-Path -LiteralPath "$GccBin/x86_64-w64-mingw32-gcc.exe")) { throw 'Pass the MinGW bin directory as -GccBin.' }
$env:Path = "$GccBin;$env:CARGO_HOME/bin;$env:Path"
