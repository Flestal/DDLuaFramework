[CmdletBinding()]
param(
    [int]$ProcessId = 0,
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\DarkestDungeon',
    [string]$SaveRoot = '',
    [string]$OutputRoot = 'D:\Documents\코덱스\DDMOD\DDLuaFramework\lightweight_captures',
    [ValidateRange(100, 5000)]
    [int]$SampleMilliseconds = 500
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-JsonFile([object]$Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Copy-DirectorySnapshot([string]$Source, [string]$Destination) {
    if (-not $Source -or -not (Test-Path -LiteralPath $Source)) { return $false }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    & robocopy.exe $Source $Destination /E /COPY:DAT /DCOPY:T /R:1 /W:1 /XJ /NP /NFL /NDL | Out-Null
    if ($LASTEXITCODE -gt 7) { throw "robocopy failed with exit code ${LASTEXITCODE}: $Source" }
    return $true
}

function Find-SaveRoot {
    $steam = Get-Process -Name steam -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $steam -or -not $steam.Path) { return '' }
    $userdata = Join-Path (Split-Path -Parent $steam.Path) 'userdata'
    foreach ($account in Get-ChildItem -LiteralPath $userdata -Directory -ErrorAction SilentlyContinue) {
        $candidate = Join-Path $account.FullName '262060\remote'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return ''
}

$started = Get-Date
$stamp = $started.ToString('yyyyMMdd_HHmmss')
$capture = Join-Path $OutputRoot $stamp
$statusPath = Join-Path $capture 'capture_status.json'
$complete = $false
$failure = $null
$gameProcess = $null

try {
    if ($ProcessId -gt 0) {
        $gameProcess = Get-Process -Id $ProcessId -ErrorAction Stop
        if ($gameProcess.ProcessName -ne 'Darkest') { throw "PID $ProcessId is not Darkest.exe." }
    } else {
        $gameProcess = Get-Process -Name Darkest -ErrorAction Stop | Select-Object -First 1
    }
    $gamePid = $gameProcess.Id
    $gameRootPath = (Resolve-Path -LiteralPath $GameRoot).Path
    if (-not $SaveRoot) { $SaveRoot = Find-SaveRoot }

    New-Item -ItemType Directory -Force -Path $capture | Out-Null
    Write-JsonFile ([ordered]@{
        state = 'INITIALIZING'
        mode = 'LIGHTWEIGHT_ATTACH'
        capture_id = $stamp
        attached = $started.ToString('o')
        game_pid = $gamePid
        scope = 'native bridge binary trace, process/thread/module samples, logs, saves, loaded module hashes, Windows application events'
        exclusions = 'No TTD instruction recording and no WPR kernel trace, to preserve playable frame rate.'
    }) $statusPath

    $inputs = Join-Path $capture 'inputs'
    New-Item -ItemType Directory -Force -Path $inputs | Out-Null
    if ($SaveRoot) { [void](Copy-DirectorySnapshot $SaveRoot (Join-Path $inputs 'saves_at_attach')) }

    $gameExe = $gameProcess.Path
    Write-JsonFile ([ordered]@{
        captured_at = (Get-Date).ToString('o')
        game = Get-Item -LiteralPath $gameExe | Select-Object FullName,Length,LastWriteTime,@{n='SHA256';e={(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash}}
        winmm = Get-Item -LiteralPath (Join-Path (Split-Path -Parent $gameExe) 'winmm.dll') | Select-Object FullName,Length,LastWriteTime,@{n='SHA256';e={(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash}}
    }) (Join-Path $capture 'system.json')

    'timestamp,pid,cpu_seconds,working_set,private_memory,thread_count,handle_count,responding' |
        Set-Content -LiteralPath (Join-Path $capture 'process_samples.csv') -Encoding utf8
    'timestamp,pid,thread_id,state,wait_reason,start_address' |
        Set-Content -LiteralPath (Join-Path $capture 'thread_samples.csv') -Encoding utf8
    'timestamp,pid,module_name,module_path,base_address,module_size' |
        Set-Content -LiteralPath (Join-Path $capture 'module_samples.csv') -Encoding utf8

    $modulePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $lastModuleSample = [datetime]::MinValue
    Write-JsonFile ([ordered]@{
        state = 'RECORDING'
        mode = 'LIGHTWEIGHT_ATTACH'
        capture_id = $stamp
        attached = $started.ToString('o')
        game_pid = $gamePid
    }) $statusPath

    while ($true) {
        $sample = Get-Process -Id $gamePid -ErrorAction SilentlyContinue
        if (-not $sample) { break }
        $now = Get-Date
        $line = '{0},{1},{2},{3},{4},{5},{6},{7}' -f $now.ToString('o'),$sample.Id,$sample.CPU,
            $sample.WorkingSet64,$sample.PrivateMemorySize64,$sample.Threads.Count,$sample.HandleCount,$sample.Responding
        Add-Content -LiteralPath (Join-Path $capture 'process_samples.csv') -Value $line -Encoding utf8
        foreach ($thread in $sample.Threads) {
            $waitReason = ''
            $startAddress = ''
            try { $waitReason = $thread.WaitReason } catch {}
            try { $startAddress = '0x{0:X}' -f $thread.StartAddress.ToInt64() } catch {}
            $threadLine = '{0},{1},{2},{3},{4},{5}' -f $now.ToString('o'),$sample.Id,$thread.Id,
                $thread.ThreadState,$waitReason,$startAddress
            Add-Content -LiteralPath (Join-Path $capture 'thread_samples.csv') -Value $threadLine -Encoding utf8
        }
        if (($now - $lastModuleSample).TotalSeconds -ge 5) {
            try {
                foreach ($module in $sample.Modules) {
                    [void]$modulePaths.Add($module.FileName)
                    $moduleLine = '{0},{1},"{2}","{3}",0x{4:X},{5}' -f $now.ToString('o'),$sample.Id,
                        $module.ModuleName.Replace('"','""'),$module.FileName.Replace('"','""'),
                        $module.BaseAddress.ToInt64(),$module.ModuleMemorySize
                    Add-Content -LiteralPath (Join-Path $capture 'module_samples.csv') -Value $moduleLine -Encoding utf8
                }
            } catch {}
            $lastModuleSample = $now
        }
        Start-Sleep -Milliseconds $SampleMilliseconds
    }

    $ended = Get-Date
    $logRoot = Join-Path $gameRootPath '_windows\win64\ddlua\logs'
    $nativeSource = Join-Path $logRoot ("native_trace_{0}.bin" -f $gamePid)
    $nativeDir = Join-Path $capture 'native_diagnostics'
    New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null
    if (-not (Test-Path -LiteralPath $nativeSource)) { throw "Native trace for PID $gamePid was not found." }
    $nativeDestination = Join-Path $nativeDir ([IO.Path]::GetFileName($nativeSource))
    Copy-Item -LiteralPath $nativeSource -Destination $nativeDestination
    if ((Get-Item -LiteralPath $nativeDestination).Length -lt 640) { throw 'Native trace contains no complete diagnostic record.' }

    $decoder = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\decode_native_trace.py'))
    & python $decoder $nativeDestination |
        Set-Content -LiteralPath (Join-Path $nativeDir ((Get-Item $nativeDestination).BaseName + '.decoder.log')) -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Native trace decoder rejected the captured trace.' }

    $appLog = Join-Path $gameRootPath 'app.log'
    $luaLog = Join-Path $logRoot 'ddlua.log'
    if (Test-Path -LiteralPath $appLog) { Copy-Item -LiteralPath $appLog -Destination (Join-Path $capture 'app.full.log') }
    if (Test-Path -LiteralPath $luaLog) { Copy-Item -LiteralPath $luaLog -Destination (Join-Path $capture 'ddlua.full.log') }
    if ($SaveRoot) { [void](Copy-DirectorySnapshot $SaveRoot (Join-Path $capture 'saves_after')) }

    $moduleRows = foreach ($path in $modulePaths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $path
        [pscustomobject]@{ original_path=$path; length=(Get-Item -LiteralPath $path).Length; sha256=$hash.Hash }
    }
    $moduleRows | Export-Csv -LiteralPath (Join-Path $capture 'loaded_modules.csv') -NoTypeInformation -Encoding utf8

    Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$started;EndTime=$ended} -ErrorAction SilentlyContinue |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,ProcessId,Message |
        Export-Csv -LiteralPath (Join-Path $capture 'windows_application_events.csv') -NoTypeInformation -Encoding utf8
    $complete = $true
}
catch {
    $failure = $_.Exception.Message
}
finally {
    if (Test-Path -LiteralPath $capture) {
        Write-JsonFile ([ordered]@{
            state = if ($complete) { 'COMPLETE' } else { 'FAILED_INCOMPLETE' }
            mode = 'LIGHTWEIGHT_ATTACH'
            capture_id = $stamp
            attached = $started.ToString('o')
            ended = (Get-Date).ToString('o')
            game_pid = if ($gameProcess) { $gameProcess.Id } else { $ProcessId }
            save_root = $SaveRoot
            failure = $failure
        }) $statusPath
    }
}

if (-not $complete) { exit 1 }
Write-Output "LIGHTWEIGHT_CAPTURE_COMPLETE=$capture"
