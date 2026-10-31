param([Parameter(Mandatory)][string]$RequestHex)
$ErrorActionPreference = 'Stop'
$taskUtf8 = [Text.UTF8Encoding]::new($false, $true)
$taskRequestId = ''
$taskResultPath = Join-Path $PSScriptRoot 'ui\action-result.json'

try {
    if ($RequestHex.Length -gt 28000 -or $RequestHex -notmatch '^(?:[0-9a-fA-F]{2})+$') {
        throw '操作内容无效，请刷新待办后重试。'
    }
    $taskRequest = $taskUtf8.GetString([Convert]::FromHexString($RequestHex)) | ConvertFrom-Json
    if ($taskRequest.id -notmatch '^[a-f0-9-]{36}$') { throw '操作编号无效。' }
    $taskRequestId = $taskRequest.id
    $taskConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'config.json') -Raw | ConvertFrom-Json
    $taskResult = @{ id = $taskRequestId; ok = $true }

    switch -Exact ($taskRequest.kind) {
        'probe' {
            if (-not [IO.File]::Exists($taskConfig.sourcePath)) { throw '找不到任务收件箱。' }
            $taskResult.message = '本地读写通道已就绪'
            if ($taskRequest.report) { $taskResult.report = $taskRequest.report }
        }
        'toggle' {
            if ($taskRequest.lineNumber -isnot [long] -and $taskRequest.lineNumber -isnot [int]) { throw '任务行号无效。' }
            if ($taskRequest.lineNumber -lt 0 -or $taskRequest.done -isnot [bool] -or $taskRequest.line -isnot [string]) {
                throw '任务内容无效，请刷新后重试。'
            }
            $taskMatch = [regex]::Match($taskRequest.line, '^(\s*(?:[-*+]|\d+[.)])\s+\[)([ xX])(\])(?:[ \t]+|$)')
            if (-not $taskMatch.Success) { throw '这行不是可勾选的任务。' }
            $taskExpected = $taskMatch.Groups[2].Value
            if (($taskRequest.done -and $taskExpected -ne ' ') -or (-not $taskRequest.done -and $taskExpected -notin @('x', 'X'))) {
                throw '任务状态已经改变，请刷新后重试。'
            }

            # Deny concurrent writes/replacement while checking and changing this one byte.
            $taskStream = [IO.File]::Open($taskConfig.sourcePath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
            try {
                $taskBytes = [byte[]]::new($taskStream.Length)
                $taskStream.ReadExactly($taskBytes)
                $taskStart = 0
                if ($taskBytes.Length -ge 3 -and $taskBytes[0] -eq 239 -and $taskBytes[1] -eq 187 -and $taskBytes[2] -eq 191) { $taskStart = 3 }
                $taskLines = [Collections.Generic.List[object]]::new()
                for ($taskI = $taskStart; $taskI -le $taskBytes.Length; $taskI++) {
                    if ($taskI -eq $taskBytes.Length -or $taskBytes[$taskI] -eq 10) {
                        $taskEnd = $taskI
                        if ($taskEnd -gt $taskStart -and $taskBytes[$taskEnd - 1] -eq 13) { $taskEnd-- }
                        $taskLines.Add(@{ offset = $taskStart; text = $taskUtf8.GetString($taskBytes, $taskStart, $taskEnd - $taskStart) })
                        $taskStart = $taskI + 1
                    }
                }
                $taskIndex = [int]$taskRequest.lineNumber
                if ($taskIndex -ge $taskLines.Count -or $taskLines[$taskIndex].text -cne $taskRequest.line) {
                    $taskMatches = @(for ($taskI = 0; $taskI -lt $taskLines.Count; $taskI++) {
                        if ($taskLines[$taskI].text -ceq $taskRequest.line) { $taskI }
                    })
                    if ($taskMatches.Count -ne 1) { throw '任务已被编辑或存在同名任务，未作修改。请刷新后重试。' }
                    $taskIndex = $taskMatches[0]
                }
                $taskCharIndex = $taskMatch.Groups[1].Length
                $taskOffset = $taskLines[$taskIndex].offset + $taskUtf8.GetByteCount($taskRequest.line.Substring(0, $taskCharIndex))
                $taskNewChar = if ($taskRequest.done) { 'x' } else { ' ' }
                $null = $taskStream.Seek($taskOffset, [IO.SeekOrigin]::Begin)
                $taskStream.WriteByte([byte][char]$taskNewChar)
                $taskStream.Flush($true)
                $taskResult.line = $taskRequest.line.Remove($taskCharIndex, 1).Insert($taskCharIndex, $taskNewChar)
                $taskResult.lineNumber = $taskIndex
            }
            finally { $taskStream.Dispose() }
        }
        'open' {
            $taskUrl = [uri]$taskRequest.url
            if (-not $taskUrl.IsAbsoluteUri -or $taskUrl.Scheme -notin @('https', 'http', 'obsidian')) { throw '此链接类型不受支持。' }
            if ($taskUrl.Scheme -eq 'obsidian') {
                if ($taskUrl.Host -ne 'open') { throw '只支持打开 Obsidian 笔记。' }
                $taskQuery = @{}
                foreach ($taskPart in $taskUrl.Query.TrimStart('?').Split('&')) {
                    $taskPair = $taskPart.Split('=', 2)
                    if ($taskPair.Count -eq 2) { $taskQuery[$taskPair[0]] = [uri]::UnescapeDataString($taskPair[1]) }
                }
                if ($taskQuery.vault -cne $taskConfig.vault -or -not $taskQuery.file -or @($taskQuery.Keys | Where-Object { $_ -notin @('vault', 'file') }).Count) {
                    throw '此链接不属于当前笔记库。'
                }
            }
            $taskStartInfo = [Diagnostics.ProcessStartInfo]::new($taskUrl.AbsoluteUri)
            $taskStartInfo.UseShellExecute = $true
            $null = [Diagnostics.Process]::Start($taskStartInfo)
        }
        default { throw '不支持的操作。' }
    }
}
catch {
    $taskResult = @{ id = $taskRequestId; ok = $false; message = $_.Exception.Message }
}

$taskResultTemp = "$taskResultPath.$([Guid]::NewGuid().ToString('N')).tmp"
[IO.File]::WriteAllText($taskResultTemp, ($taskResult | ConvertTo-Json -Compress -Depth 5), [Text.UTF8Encoding]::new($false))
[IO.File]::Move($taskResultTemp, $taskResultPath, $true)
