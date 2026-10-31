param([string]$Destination = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$File = Join-Path $Destination 'arsclib.jar'
$Hash = '0cc95b160ba4c731a5cadb455f32b79f4e64b4160bc5ec3e5183ea3676b385ce'
if (-not (Test-Path -LiteralPath $File)) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Invoke-WebRequest -Uri 'https://repo.maven.apache.org/maven2/io/github/reandroid/ARSCLib/1.4.0/ARSCLib-1.4.0.jar' -OutFile $File -TimeoutSec 180
}
if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) { throw 'ARSCLib 1.4.0 verification failed' }
Write-Output 'Verified ARSCLib 1.4.0'
