param([Parameter(Mandatory)][string]$FixtureRoot)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath($FixtureRoot)
if (Test-Path -LiteralPath $taskRoot) { throw 'Use a fresh fixture directory.' }
$null = New-Item -ItemType Directory -Path (Join-Path $taskRoot 'ui')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\resources\Action.ps1') -Destination (Join-Path $taskRoot 'Action.ps1')
$taskSource = Join-Path $taskRoot '临时任务.md'
@{ sourcePath = $taskSource; vault = 'FixtureVault' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'config.json')
$taskUtf8 = [Text.UTF8Encoding]::new($false)
$taskChecks = [Collections.Generic.List[string]]::new()
function Assert-Task($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Invoke-TaskAction($Request) {
    $Request.id = [Guid]::NewGuid().ToString()
    $taskHex = [Convert]::ToHexString($taskUtf8.GetBytes(($Request | ConvertTo-Json -Compress)))
    & (Get-Command pwsh).Source -NoProfile -NonInteractive -File (Join-Path $taskRoot 'Action.ps1') -RequestHex $taskHex
    Assert-Task ($LASTEXITCODE -eq 0) 'Backend process failed'
    return Get-Content -LiteralPath (Join-Path $taskRoot 'ui\action-result.json') -Raw | ConvertFrom-Json
}
foreach ($taskEnding in @("`n", "`r`n")) {
    foreach ($taskBom in @($false, $true)) {
        $taskText = @('# 收件箱', '', '- [ ] 中文 **重点** [[Projects/笔记|名称]] #todo', '- [ ] 保持不变') -join $taskEnding
        [IO.File]::WriteAllText($taskSource, $taskText, [Text.UTF8Encoding]::new($taskBom))
        $taskBefore = [IO.File]::ReadAllBytes($taskSource)
        $taskResult = Invoke-TaskAction @{ kind = 'toggle'; line = '- [ ] 中文 **重点** [[Projects/笔记|名称]] #todo'; lineNumber = 2; done = $true }
        Assert-Task $taskResult.ok $taskResult.message
        $taskAfter = [IO.File]::ReadAllBytes($taskSource)
        Assert-Task ($taskBefore.Length -eq $taskAfter.Length) 'Changed file length'
        $taskDifferences = @(for ($taskI = 0; $taskI -lt $taskBefore.Length; $taskI++) { if ($taskBefore[$taskI] -ne $taskAfter[$taskI]) { $taskI } })
        Assert-Task ($taskDifferences.Count -eq 1) 'Expected exactly one byte change'
        Assert-Task ($taskBefore[$taskDifferences[0]] -eq 32 -and $taskAfter[$taskDifferences[0]] -eq 120) 'Wrong byte changed'
        $taskUndo = Invoke-TaskAction @{ kind = 'toggle'; line = $taskResult.line; lineNumber = $taskResult.lineNumber; done = $false }
        Assert-Task $taskUndo.ok $taskUndo.message
        Assert-Task ([Convert]::ToHexString($taskBefore) -ceq [Convert]::ToHexString([IO.File]::ReadAllBytes($taskSource))) 'Undo did not restore bytes'
        $taskChecks.Add("one-byte write and exact undo: newline=$($taskEnding.Length), BOM=$taskBom")
    }
}
[IO.File]::WriteAllText($taskSource, "# 新插入标题`n说明`n- [ ] 移动的任务`n", $taskUtf8)
$taskResult = Invoke-TaskAction @{ kind = 'toggle'; line = '- [ ] 移动的任务'; lineNumber = 0; done = $true }
Assert-Task ($taskResult.ok -and $taskResult.lineNumber -eq 2) 'Unique moved task not found'
$taskChecks.Add('unique moved task accepted')
foreach ($taskContent in @("- [ ] 已被编辑`n", "标题`n- [ ] 原任务`n- [ ] 原任务`n")) {
    [IO.File]::WriteAllText($taskSource, $taskContent, $taskUtf8)
    $taskBeforeHash = (Get-FileHash -LiteralPath $taskSource).Hash
    $taskResult = Invoke-TaskAction @{ kind = 'toggle'; line = '- [ ] 原任务'; lineNumber = 0; done = $true }
    Assert-Task (-not $taskResult.ok) 'Conflict unexpectedly succeeded'
    Assert-Task ($taskBeforeHash -ceq (Get-FileHash -LiteralPath $taskSource).Hash) 'Conflict changed data'
}
$taskChecks.Add('edited and ambiguous moved tasks rejected without writes')
[IO.File]::WriteAllText($taskSource, '- [ ] 锁定任务', $taskUtf8)
$taskLock = [IO.File]::Open($taskSource, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $taskResult = Invoke-TaskAction @{ kind = 'toggle'; line = '- [ ] 锁定任务'; lineNumber = 0; done = $true }
    Assert-Task (-not $taskResult.ok) 'Concurrent file lock not respected'
} finally { $taskLock.Dispose() }
$taskChecks.Add('concurrent writer rejected')
foreach ($taskUrl in @('file:///C:/Windows/notepad.exe', 'javascript:alert(1)', 'obsidian://new?vault=FixtureVault&file=x', 'obsidian://open?vault=Other&file=x')) {
    $taskResult = Invoke-TaskAction @{ kind = 'open'; url = $taskUrl }
    Assert-Task (-not $taskResult.ok) 'Unsafe URL accepted'
}
$taskChecks.Add('unsupported protocols and cross-vault actions rejected')
$taskResult = Invoke-TaskAction @{ kind = 'toggle'; line = '- [ ] 锁定任务'; lineNumber = 0; done = 'false' }
Assert-Task (-not $taskResult.ok) 'Invalid boolean accepted'
$taskChecks.Add('invalid state rejected')
@{ passed = $taskChecks.Count; checks = @($taskChecks); fixtureRoot = $taskRoot } | ConvertTo-Json -Depth 4
