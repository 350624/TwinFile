param(
    [string]$BuildRoot = 'D:/TwinFile-Rust-Build',
    [string]$GccBin = 'D:/Qt/Tools/mingw1310_64/bin',
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'toolchain.ps1') -BuildRoot $BuildRoot -GccBin $GccBin
    Push-Location -LiteralPath $PSScriptRoot
    try {
        cargo build --locked --release --bin twinfile-rust
        if ($LASTEXITCODE -ne 0) { throw "Rust build failed: $LASTEXITCODE" }
        $exe = Join-Path $env:CARGO_TARGET_DIR 'release/twinfile-rust.exe'
        if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing build output: $exe" }
        if (!$BuildOnly) { Start-Process -FilePath $exe -WorkingDirectory $PSScriptRoot -WindowStyle Normal }
    } finally { Pop-Location }
} catch {
    Write-Error -ErrorAction Continue $_
    if (!$BuildOnly) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
