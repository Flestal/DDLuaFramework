param(
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\DarkestDungeon',
    [string]$OutputRoot = 'D:\Documents\코덱스\DDMOD\DDLuaFramework\captures'
)

$ErrorActionPreference = 'Stop'
$process = Get-Process -Name Darkest -ErrorAction Stop | Select-Object -First 1
$started = Get-Date
$stamp = $started.ToString('yyyyMMdd_HHmmss')
$captureDir = Join-Path $OutputRoot $stamp
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null

$appLog = Join-Path $GameRoot 'app.log'
$luaLog = Join-Path $GameRoot '_windows\win64\ddlua\logs\ddlua.log'
$appStart = if (Test-Path -LiteralPath $appLog) { (Get-Item -LiteralPath $appLog).Length } else { 0 }
$luaStart = if (Test-Path -LiteralPath $luaLog) { (Get-Item -LiteralPath $luaLog).Length } else { 0 }

"timestamp,pid,cpu_seconds,working_set,private_memory,thread_count,handle_count" | Set-Content -LiteralPath (Join-Path $captureDir 'process_samples.csv') -Encoding utf8

while (Get-Process -Id $process.Id -ErrorAction SilentlyContinue) {
    $sample = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if ($null -ne $sample) {
        $line = '{0},{1},{2},{3},{4},{5},{6}' -f (Get-Date).ToString('o'), $sample.Id, $sample.CPU, $sample.WorkingSet64, $sample.PrivateMemorySize64, $sample.Threads.Count, $sample.HandleCount
        Add-Content -LiteralPath (Join-Path $captureDir 'process_samples.csv') -Value $line -Encoding utf8
    }
    Start-Sleep -Milliseconds 1000
}

function Write-LogDelta([string]$Source, [long]$Offset, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    $stream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -ge $Offset) { [void]$stream.Seek($Offset, [IO.SeekOrigin]::Begin) }
        $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8, $true)
        try { $reader.ReadToEnd() | Set-Content -LiteralPath $Destination -Encoding utf8 } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

Write-LogDelta $appLog $appStart (Join-Path $captureDir 'app.delta.log')
Write-LogDelta $luaLog $luaStart (Join-Path $captureDir 'ddlua.delta.log')

$summary = [ordered]@{
    pid = $process.Id
    process_started = $process.StartTime.ToString('o')
    capture_started = $started.ToString('o')
    capture_ended = (Get-Date).ToString('o')
    app_log_initial_offset = $appStart
    ddlua_log_initial_offset = $luaStart
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $captureDir 'summary.json') -Encoding utf8
Write-Output "CAPTURE_COMPLETE=$captureDir"
