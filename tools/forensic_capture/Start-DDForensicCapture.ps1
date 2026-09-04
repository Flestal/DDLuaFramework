[CmdletBinding()]
param(
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\DarkestDungeon',
    [string]$SaveRoot = '',
    [string]$OutputRoot = 'D:\Documents\코덱스\DDMOD\DDLuaFramework\forensic_captures',
    [ValidateRange(50, 5000)]
    [int]$SampleMilliseconds = 250,
    [ValidateRange(10, 2000)]
    [int]$MinimumFreeGiB = 100
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-JsonFile([object]$Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Get-RelativeName([string]$Root, [string]$Path) {
    return [IO.Path]::GetRelativePath($Root, $Path)
}

function Copy-DirectorySnapshot([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return $false }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    & robocopy.exe $Source $Destination /E /COPY:DAT /DCOPY:T /R:1 /W:1 /XJ /NP /NFL /NDL | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "robocopy failed with exit code ${LASTEXITCODE}: $Source"
    }
    return $true
}

function Write-FileManifest([string]$Root, [string]$Destination) {
    $rows = foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName
        [pscustomobject]@{
            relative_path = Get-RelativeName $Root $file.FullName
            length = $file.Length
            last_write_utc = $file.LastWriteTimeUtc.ToString('o')
            sha256 = $hash.Hash
        }
    }
    $rows | Export-Csv -LiteralPath $Destination -NoTypeInformation -Encoding utf8
}

function Write-LogDelta([string]$Source, [long]$Offset, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    $stream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -ge $Offset) {
            [void]$stream.Seek($Offset, [IO.SeekOrigin]::Begin)
        }
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        try { $reader.ReadToEnd() | Set-Content -LiteralPath $Destination -Encoding utf8 }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
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

function Get-TtdPath {
    $local = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\ttd-runtime\TTD.exe'))
    if (Test-Path -LiteralPath $local) { return $local }
    $package = Get-AppxPackage -Name Microsoft.WinDbg -ErrorAction SilentlyContinue
    if ($package -and $package.InstallLocation) {
        $installed = Join-Path $package.InstallLocation 'amd64\ttd\TTD.exe'
        if (Test-Path -LiteralPath $installed) { return $installed }
    }
    throw 'Microsoft TTD.exe was not found. Run Test-DDForensicCapture.ps1 for diagnostics.'
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
    $hostExe = (Get-Process -Id $PID).Path
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
        '-GameRoot', ('"' + $GameRoot + '"'), '-SaveRoot', ('"' + $SaveRoot + '"'),
        '-OutputRoot', ('"' + $OutputRoot + '"'), '-SampleMilliseconds', $SampleMilliseconds,
        '-MinimumFreeGiB', $MinimumFreeGiB
    )
    Write-Host 'Requesting administrator rights required by Microsoft TTD and WPR...'
    Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList $arguments -WorkingDirectory $PSScriptRoot
    exit 0
}

$started = Get-Date
$stamp = $started.ToString('yyyyMMdd_HHmmss')
$capture = Join-Path $OutputRoot $stamp
$statusPath = Join-Path $capture 'capture_status.json'
$wprStarted = $false
$ttdProcess = $null
$gameProcess = $null
$complete = $false
$failure = $null

try {
    if (Get-Process -Name Darkest -ErrorAction SilentlyContinue) {
        throw 'Darkest.exe is already running. Full capture must launch it from the beginning.'
    }

    $gameRootPath = (Resolve-Path -LiteralPath $GameRoot).Path
    $gameExe = Join-Path $gameRootPath '_windows\win64\Darkest.exe'
    if (-not (Test-Path -LiteralPath $gameExe)) { throw "Darkest.exe not found: $gameExe" }
    $ttd = Get-TtdPath
    $eulaMarker = Join-Path (Split-Path -Parent $ttd) '.eula-accepted-by-user'
    if (-not (Test-Path -LiteralPath $eulaMarker)) {
        throw 'TTD EULA has not been accepted by the user. Run Accept-TtdEula.ps1 once.'
    }
    if (-not $SaveRoot) { $SaveRoot = Find-SaveRoot }

    New-Item -ItemType Directory -Force -Path $capture | Out-Null
    $driveName = ([IO.Path]::GetPathRoot($capture)).TrimEnd('\').TrimEnd(':')
    $freeBytes = (Get-PSDrive -Name $driveName).Free
    if ($freeBytes -lt $MinimumFreeGiB * 1GB) {
        throw "Capture drive has less than $MinimumFreeGiB GiB free. Full instruction traces are not safely bounded."
    }

    Write-JsonFile ([ordered]@{
        state = 'INITIALIZING'
        capture_id = $stamp
        started = $started.ToString('o')
        scope = 'Darkest.exe process tree, full TTD execution, ETW, native bridge diagnostics, inputs, saves, logs, modules'
    }) $statusPath

    $inputs = Join-Path $capture 'inputs'
    New-Item -ItemType Directory -Force -Path $inputs | Out-Null
    Write-Output 'Snapshotting the complete game tree. This can take several minutes.'
    [void](Copy-DirectorySnapshot $gameRootPath (Join-Path $inputs 'game'))
    if ($SaveRoot) {
        [void](Copy-DirectorySnapshot $SaveRoot (Join-Path $inputs 'saves_before'))
    }

    $system = [ordered]@{
        captured_at = (Get-Date).ToString('o')
        computer = $env:COMPUTERNAME
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture,LastBootUpTime
        cpu = Get-CimInstance Win32_Processor | Select-Object Name,Manufacturer,NumberOfCores,NumberOfLogicalProcessors
        gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate)
        ttd = Get-Item -LiteralPath $ttd | Select-Object FullName,Length,@{n='Version';e={$_.VersionInfo.FileVersion}}
        game = Get-Item -LiteralPath $gameExe | Select-Object FullName,Length,LastWriteTime,@{n='SHA256';e={(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash}}
    }
    Write-JsonFile $system (Join-Path $capture 'system.json')

    $appLog = Join-Path $gameRootPath 'app.log'
    $luaLog = Join-Path $gameRootPath '_windows\win64\ddlua\logs\ddlua.log'
    $nativeLogRoot = Join-Path $gameRootPath '_windows\win64\ddlua\logs'
    $nativeBefore = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if (Test-Path -LiteralPath $nativeLogRoot) {
        Get-ChildItem -LiteralPath $nativeLogRoot -Filter 'native_trace_*.bin' -File |
            ForEach-Object { [void]$nativeBefore.Add($_.FullName) }
    }
    $appOffset = if (Test-Path -LiteralPath $appLog) { (Get-Item -LiteralPath $appLog).Length } else { 0 }
    $luaOffset = if (Test-Path -LiteralPath $luaLog) { (Get-Item -LiteralPath $luaLog).Length } else { 0 }

    $wprStatus = (& wpr.exe -status 2>&1 | Out-String)
    if ($wprStatus -notmatch 'not recording|is not recording|WPR is not recording') {
        throw 'Another WPR recording appears to be active; refusing to overwrite or cancel it.'
    }
    & wpr.exe -start GeneralProfile -filemode | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "WPR start failed with exit code $LASTEXITCODE" }
    $wprStarted = $true

    $traceDir = Join-Path $capture 'ttd'
    New-Item -ItemType Directory -Force -Path $traceDir | Out-Null
    $tracePath = Join-Path $traceDir 'Darkest.run'
    $ttdLog = Join-Path $capture 'ttd_console.log'
    $ttdArgs = @('-accepteula', '-out', ('"' + $tracePath + '"'), '-children',
        '-noUI', '-launch', ('"' + $gameExe + '"'))
    $ttdProcess = Start-Process -FilePath $ttd -ArgumentList $ttdArgs -WorkingDirectory (Split-Path -Parent $gameExe) `
        -RedirectStandardOutput $ttdLog -RedirectStandardError (Join-Path $capture 'ttd_error.log') -PassThru

    $deadline = (Get-Date).AddMinutes(3)
    while ((Get-Date) -lt $deadline -and -not $ttdProcess.HasExited) {
        $gameProcess = Get-Process -Name Darkest -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($gameProcess) { break }
        Start-Sleep -Milliseconds 100
        $ttdProcess.Refresh()
    }
    if (-not $gameProcess) { throw 'TTD did not launch Darkest.exe within three minutes.' }

    'timestamp,pid,cpu_seconds,working_set,private_memory,thread_count,handle_count,responding' |
        Set-Content -LiteralPath (Join-Path $capture 'process_samples.csv') -Encoding utf8
    'timestamp,pid,thread_id,state,wait_reason,start_address' |
        Set-Content -LiteralPath (Join-Path $capture 'thread_samples.csv') -Encoding utf8
    'timestamp,pid,module_name,module_path,base_address,module_size' |
        Set-Content -LiteralPath (Join-Path $capture 'module_samples.csv') -Encoding utf8

    $modulePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $lastModuleSample = [datetime]::MinValue
    while ($true) {
        $sample = Get-Process -Id $gameProcess.Id -ErrorAction SilentlyContinue
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
        if (($now - $lastModuleSample).TotalSeconds -ge 2) {
            foreach ($module in $sample.Modules) {
                [void]$modulePaths.Add($module.FileName)
                $moduleLine = '{0},{1},"{2}","{3}",0x{4:X},{5}' -f $now.ToString('o'),$sample.Id,
                    $module.ModuleName.Replace('"','""'),$module.FileName.Replace('"','""'),
                    $module.BaseAddress.ToInt64(),$module.ModuleMemorySize
                Add-Content -LiteralPath (Join-Path $capture 'module_samples.csv') -Value $moduleLine -Encoding utf8
            }
            $lastModuleSample = $now
        }
        Start-Sleep -Milliseconds $SampleMilliseconds
    }

    $ttdProcess.WaitForExit(120000)
    $ttdProcess.Refresh()
    if (-not $ttdProcess.HasExited) { throw 'TTD did not finalize within 120 seconds after game exit.' }
    if ($ttdProcess.ExitCode -ne 0) { throw "TTD exited with code $($ttdProcess.ExitCode)." }

    & wpr.exe -stop (Join-Path $capture 'system.etl') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "WPR stop failed with exit code $LASTEXITCODE" }
    $wprStarted = $false

    Write-LogDelta $appLog $appOffset (Join-Path $capture 'app.delta.log')
    Write-LogDelta $luaLog $luaOffset (Join-Path $capture 'ddlua.delta.log')
    $luaDeltaText = if (Test-Path -LiteralPath (Join-Path $capture 'ddlua.delta.log')) {
        Get-Content -LiteralPath (Join-Path $capture 'ddlua.delta.log') -Raw
    } else { '' }
    if ($luaDeltaText -notmatch 'Native all-call diagnostic trace opened' -or
        $luaDeltaText -notmatch 'Generic Lua effect bridge installed') {
        throw 'DDLua did not confirm that the native diagnostic and effect bridges were active.'
    }
    if (Test-Path -LiteralPath $appLog) { Copy-Item -LiteralPath $appLog -Destination (Join-Path $capture 'app.full.log') }
    if (Test-Path -LiteralPath $luaLog) { Copy-Item -LiteralPath $luaLog -Destination (Join-Path $capture 'ddlua.full.log') }
    if ($SaveRoot) { [void](Copy-DirectorySnapshot $SaveRoot (Join-Path $capture 'saves_after')) }

    $nativeCaptureRoot = Join-Path $capture 'native_diagnostics'
    New-Item -ItemType Directory -Force -Path $nativeCaptureRoot | Out-Null
    $nativeTraces = @()
    if (Test-Path -LiteralPath $nativeLogRoot) {
        $nativeTraces = @(Get-ChildItem -LiteralPath $nativeLogRoot -Filter 'native_trace_*.bin' -File |
            Where-Object { -not $nativeBefore.Contains($_.FullName) -and $_.LastWriteTime -ge $started.AddMinutes(-1) })
    }
    if ($nativeTraces.Count -eq 0) { throw 'No native all-call diagnostic trace was produced.' }
    $decoder = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\decode_native_trace.py'))
    if (-not (Test-Path -LiteralPath $decoder)) { throw "Native trace decoder missing: $decoder" }
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python is required to validate native diagnostic traces.' }
    foreach ($nativeTrace in $nativeTraces) {
        $copiedTrace = Join-Path $nativeCaptureRoot $nativeTrace.Name
        Copy-Item -LiteralPath $nativeTrace.FullName -Destination $copiedTrace
        & python $decoder $copiedTrace |
            Set-Content -LiteralPath (Join-Path $nativeCaptureRoot ($nativeTrace.BaseName + '.decoder.log')) -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "Native trace decoder rejected $($nativeTrace.Name)" }
    }

    $moduleDir = Join-Path $inputs 'loaded_modules'
    New-Item -ItemType Directory -Force -Path $moduleDir | Out-Null
    $moduleRows = foreach ($path in $modulePaths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $path
        $destination = Join-Path $moduleDir ($hash.Hash + '_' + [IO.Path]::GetFileName($path))
        if (-not (Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $path -Destination $destination }
        [pscustomobject]@{ original_path=$path; sha256=$hash.Hash; captured_file=[IO.Path]::GetFileName($destination) }
    }
    $moduleRows | Export-Csv -LiteralPath (Join-Path $capture 'loaded_modules.csv') -NoTypeInformation -Encoding utf8

    Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$started;EndTime=(Get-Date)} -ErrorAction SilentlyContinue |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,ProcessId,Message |
        Export-Csv -LiteralPath (Join-Path $capture 'windows_application_events.csv') -NoTypeInformation -Encoding utf8

    $runFiles = @(Get-ChildItem -LiteralPath $traceDir -Filter '*.run' -File)
    $outFiles = @(Get-ChildItem -LiteralPath $traceDir -Filter '*.out' -File)
    if ($runFiles.Count -eq 0 -or ($runFiles | Measure-Object Length -Sum).Sum -le 0) {
        throw 'TTD produced no non-empty .run trace.'
    }
    if ($outFiles.Count -eq 0) { throw 'TTD produced no .out diagnostics file.' }

    $complete = $true
}
catch {
    $failure = $_.Exception.Message
    Write-Warning $failure
}
finally {
    if ($wprStarted) {
        & wpr.exe -stop (Join-Path $capture 'system.partial.etl') 2>&1 | Out-Null
    }
    $ended = Get-Date
    if (Test-Path -LiteralPath $capture) {
        Write-JsonFile ([ordered]@{
            state = if ($complete) { 'COMPLETE' } else { 'FAILED_INCOMPLETE' }
            capture_id = $stamp
            started = $started.ToString('o')
            ended = $ended.ToString('o')
            failure = $failure
            ttd_exit_code = if ($ttdProcess -and $ttdProcess.HasExited) { $ttdProcess.ExitCode } else { $null }
            game_pid = if ($gameProcess) { $gameProcess.Id } else { $null }
            save_root = $SaveRoot
        }) $statusPath
        if ($complete) {
            try {
                Write-FileManifest $capture (Join-Path $capture 'artifact_manifest.csv')
            }
            catch {
                $complete = $false
                $failure = "Artifact manifest failed: $($_.Exception.Message)"
                Write-JsonFile ([ordered]@{
                    state = 'FAILED_INCOMPLETE'
                    capture_id = $stamp
                    started = $started.ToString('o')
                    ended = (Get-Date).ToString('o')
                    failure = $failure
                    ttd_exit_code = if ($ttdProcess -and $ttdProcess.HasExited) { $ttdProcess.ExitCode } else { $null }
                    game_pid = if ($gameProcess) { $gameProcess.Id } else { $null }
                    save_root = $SaveRoot
                }) $statusPath
            }
        }
    }
}

if (-not $complete) { exit 1 }
Write-Output "FORENSIC_CAPTURE_COMPLETE=$capture"
