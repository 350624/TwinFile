param([string]$Destination = (Join-Path $PSScriptRoot 'slides'))
$ErrorActionPreference = 'Stop'
$Lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'slides-dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($Dependency in $Lock) {
    $Name = "$($Dependency.artifact)-$($Dependency.version).jar"
    $File = Join-Path $Destination $Name
    if (Test-Path -LiteralPath $File) {
        if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Dependency.sha256) { continue }
        throw "Existing slide dependency has an unexpected hash: $Name"
    }
    $Uri = "https://repo.maven.apache.org/maven2/$($Dependency.group.Replace('.','/'))/$($Dependency.artifact)/$($Dependency.version)/$Name"
    Invoke-WebRequest -Uri $Uri -OutFile $File -TimeoutSec 180
    if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Dependency.sha256) { throw "Slide dependency verification failed: $Name" }
    Write-Output "Verified $Name"
}
