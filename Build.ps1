param([Parameter(Mandatory)][string]$WebView2Sdk)
$ErrorActionPreference = 'Stop'
$taskSdk = (Resolve-Path -LiteralPath $WebView2Sdk).Path
$taskRoot = $PSScriptRoot
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVs) { throw 'Visual C++ Build Tools are required.' }
$taskVcvars = Join-Path $taskVs 'VC\Auxiliary\Build\vcvars64.bat'
$taskInclude = Join-Path $taskSdk 'build\native\include'
$taskLoader = Join-Path $taskSdk 'build\native\x64\WebView2LoaderStatic.lib'
if (-not (Test-Path -LiteralPath $taskLoader)) { throw 'WebView2 SDK x64 loader is missing.' }
$taskBuild = Join-Path $taskRoot 'build'
$null = New-Item -ItemType Directory -Path $taskBuild -Force
$taskCommand = @"
@echo off
call "$taskVcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"$taskInclude" "$taskRoot\src\App.cpp" "$taskRoot\src\Renderer.cpp" /Fe:"$taskBuild\NativeGlassTasks.exe" /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT "$taskLoader" d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib windowsapp.lib user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib version.lib dwmapi.lib wtsapi32.lib
exit /b %errorlevel%
"@
$taskCmdPath = Join-Path $taskBuild 'compile.cmd'
[IO.File]::WriteAllText($taskCmdPath, ($taskCommand -replace "`r?`n", "`r`n"), [Text.Encoding]::Default)
Push-Location -LiteralPath $taskBuild
try {
    & $env:ComSpec /d /c $taskCmdPath
    if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $taskRoot 'resources') -Destination $taskBuild -Recurse -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'src\Glass.hlsl') -Destination (Join-Path $taskBuild 'resources\Glass.hlsl')
Get-FileHash -LiteralPath (Join-Path $taskBuild 'NativeGlassTasks.exe')
