param([switch]$SkipStop, [string]$RainmeterPath)
$ErrorActionPreference = 'Stop'
$taskExe = Join-Path $PSScriptRoot 'NativeGlassTasks.exe'
$taskRainmeter = $RainmeterPath
if (-not $taskRainmeter) {
    $taskCommand = Get-Command Rainmeter.exe -ErrorAction SilentlyContinue
    if ($taskCommand) { $taskRainmeter = $taskCommand.Source }
}
if (-not $taskRainmeter -or -not (Test-Path -LiteralPath $taskRainmeter -PathType Leaf)) { throw 'Provide -RainmeterPath with the path to Rainmeter.exe, or add Rainmeter to PATH.' }
if (-not $SkipStop) {
    $taskProcesses = @(Get-Process -Name NativeGlassTasks -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $taskExe })
    if ($taskProcesses.Count) {
        $taskSignal = Start-Process -FilePath $taskExe -ArgumentList '--stop' -WindowStyle Hidden -PassThru
        $taskSignal.WaitForExit()
        foreach ($taskProcess in $taskProcesses) {
            if (-not $taskProcess.WaitForExit(10000)) { throw 'A note operation is still active. Wait for it to finish before restoring.' }
        }
    }
}
$taskLink = Join-Path ([Environment]::GetFolderPath('Startup')) 'Obsidian Glass Tasks.lnk'
if (Test-Path -LiteralPath $taskLink) {
    $taskShell = New-Object -ComObject WScript.Shell
    $taskShortcut = $taskShell.CreateShortcut($taskLink)
    if ($taskShortcut.TargetPath -eq $taskExe) { Remove-Item -LiteralPath $taskLink }
}
Start-Process -FilePath $taskRainmeter -ArgumentList '[!ActivateConfig "MarkdownTasks\Glass" "Glass.ini"]' -WindowStyle Hidden
Write-Output 'Rainmeter glass tasks restored. Other skins and all notes were left untouched.'
