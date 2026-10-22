$ErrorActionPreference = 'Stop'
$Source = Split-Path -Parent $PSScriptRoot
$FixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ("TwinFile-release-tests-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
$Passed = 0
function Invoke-FixtureGit {
    param([string]$Directory,[string[]]$Arguments)
    $null = & git -C $Directory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Fixture Git command failed' }
}
function Test-Preflight {
    param([string]$Name,[scriptblock]$Arrange,[string]$ExpectedError='')
    $Directory = Join-Path $FixtureRoot $Name
    New-Item -ItemType Directory -Path $Directory | Out-Null
    Copy-Item -LiteralPath "$Source\publish.ps1" -Destination $Directory
    'project(TwinFileNative VERSION 3.1.1 LANGUAGES C CXX RC)' | Set-Content -LiteralPath "$Directory\CMakeLists.txt"
    "#define MyAppVersion `"3.1.1`"`nVersionInfoVersion=3.1.1.0" | Set-Content -LiteralPath "$Directory\installer.iss"
    "## [3.1.1] - 2026-10-22`n`nA substantive release note with actual user-visible changes and verification limits." | Set-Content -LiteralPath "$Directory\CHANGELOG.md"
    Invoke-FixtureGit $Directory @('init','-b','main')
    Invoke-FixtureGit $Directory @('config','user.name','TwinFile Test')
    Invoke-FixtureGit $Directory @('config','user.email','test@example.invalid')
    Invoke-FixtureGit $Directory @('remote','add','origin','https://github.com/350624/TwinFile.git')
    & $Arrange $Directory
    Invoke-FixtureGit $Directory @('add','--all')
    Invoke-FixtureGit $Directory @('commit','-m','Fixture')
    if ($Name -eq 'dirty') { 'private' | Set-Content -LiteralPath "$Directory\untracked.txt" }
    if ($Name -eq 'branch') { Invoke-FixtureGit $Directory @('checkout','-b','feature') }
    if ($Name -eq 'detached') { Invoke-FixtureGit $Directory @('checkout','--detach') }
    $Failure = ''
    try { $null = & "$Directory\publish.ps1" -CheckOnly } catch { $Failure = $_.Exception.Message }
    if ($ExpectedError) {
        if ($Failure -notlike "*$ExpectedError*") { throw "${Name}: expected '$ExpectedError', got '$Failure'" }
    } elseif ($Failure) { throw "${Name}: $Failure" }
    $script:Passed++
    Write-Output "PASS $Name"
}
Test-Preflight 'valid' {}
Test-Preflight 'installer-version' { param($Directory) '#define MyAppVersion "3.1.2"' | Set-Content -LiteralPath "$Directory\installer.iss" } 'versions differ'
Test-Preflight 'missing-notes' { param($Directory) '# Changelog' | Set-Content -LiteralPath "$Directory\CHANGELOG.md" } 'changelog section'
Test-Preflight 'empty-notes' { param($Directory) "## [3.1.1] - 2026-10-22`nshort" | Set-Content -LiteralPath "$Directory\CHANGELOG.md" } 'changelog section'
Test-Preflight 'wrong-origin' { param($Directory) Invoke-FixtureGit $Directory @('remote','set-url','origin','https://github.com/example/unrelated.git') } 'Unexpected origin'
Test-Preflight 'wrong-push-origin' { param($Directory) Invoke-FixtureGit $Directory @('remote','set-url','--push','origin','https://github.com/example/unrelated.git') } 'Unexpected origin'
Test-Preflight 'dirty' {} 'clean worktree'
Test-Preflight 'branch' {} 'main only'
Test-Preflight 'detached' {} 'main only'
Test-Preflight 'invalid-version' { param($Directory) 'project(Other VERSION 1)' | Set-Content -LiteralPath "$Directory\CMakeLists.txt" } 'Cannot read project version'
Write-Output "Release preflight: $Passed passed, 0 failed (local fixtures, no network or upload)"
