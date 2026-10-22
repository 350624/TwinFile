param([Parameter(Mandatory=$true)][string]$Destination, [switch]$IncludeLocalArtifacts)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw 'Choose a new destination; existing files are never overwritten' }
New-Item -ItemType Directory -Path $Destination | Out-Null
foreach ($Name in @('src','sdk','assets','tests','vendor','licenses','docs','CMakeLists.txt','build.ps1','verify.ps1','verify-ui.ps1','package.ps1','publish.ps1','export-source.ps1','installer.iss','README.md','CHANGELOG.md','AGENTS.md','MIGRATION.md','THIRD_PARTY.md','VERIFICATION.md','PLUGIN_SDK.md','LICENSE','NOTICE.md','CONTRIBUTING.md','SECURITY.md','.gitignore','.gitattributes')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $Name) -Destination $Destination -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $Destination 'tools') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'tools\README.md') -Destination (Join-Path $Destination 'tools')
if ($IncludeLocalArtifacts) {
foreach ($Name in @('tools','releases','verification','dist')) {
    $Source = Join-Path $PSScriptRoot $Name
    if (Test-Path -LiteralPath $Source) {
        $Target = Join-Path $Destination $Name
        New-Item -ItemType Directory -Path $Target -Force | Out-Null
        Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Target -Recurse -Force
    }
}
}
$Forbidden = @(Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object { $_.Name -match '(?i)(\.py[co]?$|^python.*\.(dll|exe)$|pyside|shiboken)' })
if ($Forbidden.Count) { throw "Python artifacts found: $($Forbidden.FullName -join ', ')" }
Write-Output "Standalone native project: $Destination"
