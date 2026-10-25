param([string]$Runtime = (Join-Path $PSScriptRoot 'dist\TwinFile'), [Parameter(Mandatory=$true)][string]$QtRoot)
$ErrorActionPreference = 'Stop'
$Executable = Join-Path $PSScriptRoot 'build\bin\twinfile_migration_tests.exe'
if (-not (Test-Path -LiteralPath $Executable)) { throw 'Build the native regression executable first' }
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'bin\Qt6Test.dll'))) { throw 'Pass -QtRoot for the matching QtTest development runtime' }
$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
$Evidence = Join-Path $PSScriptRoot 'verification'
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Previous = @{}
foreach ($Name in @('Path','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','TWINFILE_VERIFY_SCREENSHOT_ROOT')) { $Previous[$Name] = [Environment]::GetEnvironmentVariable($Name) }
try {
    $env:Path = "$Runtime;$QtRoot\bin;$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $Runtime
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Runtime 'platforms'
    $env:TWINFILE_VERIFY_SCREENSHOT_ROOT = $Evidence
    $Results = Join-Path $Evidence 'ui-test-results.txt'
    $Process = Start-Process -FilePath $Executable -ArgumentList @('-platform','windows','comparisonDialogsAndThemesRenderWithoutUserSettings','-o',('"' + $Results + ',txt"')) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Native UI check timed out' }
    Get-Content -LiteralPath $Results
    if ($Process.ExitCode -ne 0) { throw 'Native UI check failed' }
    $PayloadResults = Join-Path $Evidence 'payload-ui-test-results.txt'
    $Process = Start-Process -FilePath $Executable -ArgumentList @('-platform','windows','payloadDialogsAndProgress','payloadOutputDirectoryAndInlineCompletion','inlineTaskSpeedCancellationAndFailure','-o',('"' + $PayloadResults + ',txt"')) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Payload UI check timed out' }
    Get-Content -LiteralPath $PayloadResults
    if ($Process.ExitCode -ne 0) { throw 'Payload UI check failed' }
    $AnimationExecutable = Join-Path $PSScriptRoot 'build\bin\twinfile_tests.exe'
    $AnimationResults = Join-Path $Evidence 'animation-ui-test-results.txt'
    $AnimationTests = @('directoryTransitionsFollowNavigationAndArchiveParents','refreshAnimationPreservesSelectionAndScroll','reducedMotionCancelsCurrentAndPendingAnimations','viewAnimationCancelsOnInteractionResizeHideAndTheme','animationsOnlyRepaintVisibleRows','paneAnimationsRenderWithoutUserSettings')
    $Process = Start-Process -FilePath $AnimationExecutable -ArgumentList (@('-platform','windows') + $AnimationTests + @('-o',('"' + $AnimationResults + ',txt"'))) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Native animation UI check timed out' }
    Get-Content -LiteralPath $AnimationResults
    if ($Process.ExitCode -ne 0) { throw 'Native animation UI check failed' }
    $IconResults = Join-Path $Evidence 'icon-ui-test-results.txt'
    $IconTests = @('nativeResourceTables','androidStringPoolsSupportModifiedUtf8','adaptiveIconIgnoresMonochromeAndRejectsMissingLayers','webpApkIconsDecodeWithoutQtPluginOrAapt','thumbnailCacheTracksIdentityAndOnlyHandlesSupportedFiles','windowsExecutableIconsAcceptQtPaths','thumbnailResultsArrivePerFileAndSurviveFolderRevisit','fileIconsUseRoundedMasksAndCorrectFit','maskedFileIconsRenderWithoutUserSettings','dexEditorRendersAndEdits')
    $Process = Start-Process -FilePath $AnimationExecutable -ArgumentList (@('-platform','windows') + $IconTests + @('-o',('"' + $IconResults + ',txt"'))) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(90000)) { $Process.Kill(); throw 'Native icon and DEX UI check timed out' }
    Get-Content -LiteralPath $IconResults
    if ($Process.ExitCode -ne 0) { throw 'Native icon UI check failed' }
    $ExtensionExecutable = Join-Path $PSScriptRoot 'build\bin\twinfile_extension_tests.exe'
    $ExtensionResults = Join-Path $Evidence 'extension-ui-test-results.txt'
    $ExtensionTests = @('sidebarRendersAndDoesNotStealFileShortcuts','appearanceMigrationAndPalette','appearanceRendersAllModesAndRestores','propertiesRenderAndSpaceOpensProperties','contextMenusMatchSelectedEntryTypes','toolbarSelectionAndDirection','toolbarTransfersFollowActivePane','workspaceRedesignLayoutAndNavigation','workspaceNavigationScrollsWithoutGrowingWindow','storageLocationsAndTestModeIsolation','deviceAndDriveShortcuts','modelsEndpointAndExplicitGet','modelsFailures','modelsCancelAndRestart','modelSettingsDialogFetchAndManualFallback','codexComposerRendersAllModes','codexComposerMenusKeyboardAndStop','updateReleaseValidation','updateCheckIsAsyncBoundedAndShowsReleaseNotes','applicationChecksUpdatesAtStartupAndFromSettings','updateCheckFailureCancelAndStartupOnce')
    $Process = Start-Process -FilePath $ExtensionExecutable -ArgumentList (@('-platform','windows') + $ExtensionTests + @('-o',('"' + $ExtensionResults + ',txt"'))) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(60000)) { $Process.Kill(); throw 'AI and extension UI check timed out' }
    Get-Content -LiteralPath $ExtensionResults
    if ($Process.ExitCode -ne 0) { throw 'AI and extension UI check failed' }
    $AgentExecutable = Join-Path $PSScriptRoot 'build\bin\twinfile_agent_tests.exe'
    $AgentResults = Join-Path $Evidence 'agent-ui-test-results.txt'
    $Process = Start-Process -FilePath $AgentExecutable -ArgumentList @('-platform','windows','panelAgentAuthorizationApprovalAndChatIsolation','fullAccessPanelConsentAndReset','-o',('"' + $AgentResults + ',txt"')) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Agent UI check timed out' }
    Get-Content -LiteralPath $AgentResults
    if ($Process.ExitCode -ne 0) { throw 'Agent UI check failed' }
    $McpExecutable = Join-Path $PSScriptRoot 'build\bin\twinfile_mcp_tests.exe'
    $McpResults = Join-Path $Evidence 'mcp-ui-test-results.txt'
    $Process = Start-Process -FilePath $McpExecutable -ArgumentList @('-platform','windows','settingsDialogRendersAndStarts','-o',('"' + $McpResults + ',txt"')) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'MCP UI check timed out' }
    Get-Content -LiteralPath $McpResults
    if ($Process.ExitCode -ne 0) { throw 'MCP UI check failed' }
    $DownloadExecutable = Join-Path $PSScriptRoot 'build\bin\twinfile_download_tests.exe'
    $DownloadResults = Join-Path $Evidence 'download-ui-test-results.txt'
    $Process = Start-Process -FilePath $DownloadExecutable -ArgumentList @('-platform','windows','dialogRendersAndOperates','closeConfirmsCancellation','windowMenuOpensSingleDialog','windowDownloadUsesInlineProgressAndRemembersDestination','recoveryStatusSurvivesProgressUpdates','-o',('"' + $DownloadResults + ',txt"')) -WindowStyle Hidden -PassThru
    if (-not $Process.WaitForExit(30000)) { $Process.Kill(); throw 'Direct download UI check timed out' }
    Get-Content -LiteralPath $DownloadResults
    if ($Process.ExitCode -ne 0) { throw 'Direct download UI check failed' }
} finally {
    foreach ($Name in $Previous.Keys) { [Environment]::SetEnvironmentVariable($Name, $Previous[$Name]) }
}
