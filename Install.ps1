param(
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\ObsidianGlassTasks'),
    [string]$RainmeterIniPath
)
$ErrorActionPreference = 'Stop'
$taskInstall = [IO.Path]::GetFullPath($InstallDirectory)
if (Test-Path -LiteralPath $taskInstall) { throw 'Install directory already exists. Preserve and inspect it before replacing any files.' }
$taskBuild = Join-Path $PSScriptRoot 'build'
if (-not (Test-Path -LiteralPath (Join-Path $taskBuild 'NativeGlassTasks.exe'))) { throw 'Build the native app first.' }
if (-not (Test-Path -LiteralPath (Join-Path $taskBuild 'resources\config.json'))) { throw 'Create resources/config.json from the example and rebuild before installing.' }
if ($RainmeterIniPath -and -not (Test-Path -LiteralPath $RainmeterIniPath -PathType Leaf)) { throw 'The specified Rainmeter.ini does not exist.' }
$null = New-Item -ItemType Directory -Path $taskInstall
Copy-Item -LiteralPath (Join-Path $taskBuild 'NativeGlassTasks.exe') -Destination $taskInstall
Copy-Item -LiteralPath (Join-Path $taskBuild 'resources') -Destination $taskInstall -Recurse
foreach ($taskFile in @('Restore-Rainmeter.ps1','README.md','THIRD-PARTY-NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskFile) -Destination $taskInstall
}
if ($RainmeterIniPath) {
    $taskBackup = Join-Path $taskInstall 'backup'
    $null = New-Item -ItemType Directory -Path $taskBackup
    Copy-Item -LiteralPath $RainmeterIniPath -Destination (Join-Path $taskBackup 'Rainmeter.before-native.ini')
}
Write-Output "Installed at $taskInstall. Run the app and verify diagnostics before switching skins or enabling startup."
