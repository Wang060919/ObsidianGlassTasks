param(
    [Parameter(Mandatory)][string]$FixtureRoot,
    [string]$ExecutablePath = (Join-Path $PSScriptRoot '..\build\NativeGlassTasks.exe')
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath($FixtureRoot)
$taskExe = (Resolve-Path -LiteralPath $ExecutablePath).Path
if (Test-Path -LiteralPath $taskRoot) { throw 'Use a fresh fixture directory.' }
$null = New-Item -ItemType Directory -Path $taskRoot
$taskData = Join-Path $taskRoot 'data'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\resources') -Destination $taskData -Recurse
$taskSource = Join-Path $taskData 'fixture.md'
$taskText = "# Fixture`r`n`r`n- [ ] **Chinese 中文** [[Projects/Note|note]] #todo`r`n- [ ] [link](https://example.com) #todo`r`n- [ ] third task`r`n"
[IO.File]::WriteAllText($taskSource,$taskText,[Text.UTF8Encoding]::new($true))
$taskBefore = (Get-FileHash -LiteralPath $taskSource).Hash
$taskConfig = @{vault='FixtureVault';noteFile='fixture.md';sourcePath=$taskSource;powershellPath=(Get-Command pwsh).Source;testFixture=$true}
function Save-TaskConfig {
    [IO.File]::WriteAllText((Join-Path $taskData 'config.json'),($taskConfig | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}
function Invoke-NativeCheck([string]$Name,[string[]]$Arguments,[bool]$ExpectSuccess) {
    $taskReport = Join-Path $taskRoot ($Name + '.json')
    $taskProcess = Start-Process -FilePath $taskExe -ArgumentList ($Arguments + @('--diagnostics',('"' + $taskReport + '"'))) -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(50000)) { throw "Native check timed out: $Name (PID $($taskProcess.Id))" }
    $taskProcess.Refresh()
    $taskResult = Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
    if (($taskProcess.ExitCode -eq 0) -ne $ExpectSuccess) { throw "Unexpected native exit for $Name`: $($taskResult.fatal)" }
    return $taskResult
}
Save-TaskConfig
$taskRenderer = Invoke-NativeCheck 'renderer' @('--self-test') $true
if (-not $taskRenderer.ok -or $taskRenderer.refractionChangedPixels -le 200 -or $taskRenderer.centerDifference -ne 0) { throw 'Renderer contract failed' }
if ($taskRenderer.maxQueuedFrames -ne 1 -or $taskRenderer.refreshRateHz -le 1) { throw 'Low-latency presentation contract failed' }
if (-not $taskRenderer.darkMaterial -or $taskRenderer.fineTextureResidual -gt 2) { throw 'Fixed dark material contract failed' }
if ($taskRenderer.mediumTextureContrast -lt 100 -or $taskRenderer.whiteBackdropCenterPeak -lt 200 -or $taskRenderer.whiteBackdropCenterPeak -gt 236) { throw 'Clear material lost background detail or transmission' }
if ($taskRenderer.cornerGradientReversal -gt 2) { throw 'Rounded corner refraction folds a smooth backdrop' }
$taskBridgeArguments = @('--fixture-test','--data-dir',('"' + $taskData + '"'))
$taskBridge = Invoke-NativeCheck 'bridge' $taskBridgeArguments $true
if (-not $taskBridge.ui.fixtureResult.ok -or $taskBefore -cne (Get-FileHash -LiteralPath $taskSource).Hash) { throw 'Native writeback/undo changed original bytes' }
if ($taskBridge.timing.frameWakeCount -lt 20) { throw 'Frame clock stopped while WebView2 was active' }
if ($taskBridge.ui.colorScheme -ne 'dark') { throw 'The UI did not use its fixed dark palette' }
function Get-TaskLuminance([string]$Color) {
    $taskComponents = [regex]::Matches($Color,'\d+') | Select-Object -First 3 | ForEach-Object {
        $taskValue=[double]$_.Value/255
        if ($taskValue -le 0.04045) { $taskValue/12.92 } else { [math]::Pow(($taskValue+0.055)/1.055,2.4) }
    }
    if ($taskComponents.Count -ne 3) { throw 'Unexpected computed text color' }
    return 0.2126*$taskComponents[0]+0.7152*$taskComponents[1]+0.0722*$taskComponents[2]
}
$taskBodyContrast = ((Get-TaskLuminance $taskBridge.ui.textColor)+0.05)/($taskRenderer.whiteReadingMaxLuminance+0.05)
$taskSecondaryContrast = ((Get-TaskLuminance $taskBridge.ui.secondaryTextColor)+0.05)/($taskRenderer.whiteReadingMaxLuminance+0.05)
# Higher transmission is explicitly preferred. Keep the raw contrast measurement
# observable; it excludes the text shadow and is not a visual-acceptance claim.
$taskConfig.testFixture = $false
Save-TaskConfig
$taskGuard = Invoke-NativeCheck 'missing-fixture-flag' $taskBridgeArguments $false
if ($taskGuard.fatal -ne 'Fixture flag is required') { throw 'Fixture flag guard did not run' }
$taskOutside = Join-Path $taskRoot 'outside.md'
[IO.File]::WriteAllText($taskOutside,'- [ ] outside fixture',[Text.UTF8Encoding]::new($false))
$taskOutsideHash = (Get-FileHash -LiteralPath $taskOutside).Hash
$taskConfig.testFixture = $true
$taskConfig.sourcePath = $taskOutside
Save-TaskConfig
$taskGuard = Invoke-NativeCheck 'outside-fixture' $taskBridgeArguments $false
if ($taskGuard.fatal -ne 'Fixture source must be inside its isolated data directory' -or $taskOutsideHash -cne (Get-FileHash -LiteralPath $taskOutside).Hash) { throw 'Fixture path guard failed' }
@{passed=4;renderer=$taskRenderer;bridge=$taskBridge.ui.fixtureResult;frameClock=$taskBridge.timing.frameWakeInterval;contrast=@{body=$taskBodyContrast;secondary=$taskSecondaryContrast;scope='Raw colors over white backdrop, excluding text shadow';rawColorsMeetAA=($taskBodyContrast -ge 4.5 -and $taskSecondaryContrast -ge 4.5)};fixtureFlagGuard=$true;fixturePathGuard=$true;originalBytesRestored=$true} | ConvertTo-Json -Depth 5
