[CmdletBinding()]
param(
    [string]$OutputDirectory = 'D:\Documents\코덱스\DDMOD\DDLuaFramework\forensic_selftest'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdministrator) {
    $hostExe = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $PSCommandPath + '"'),
        '-OutputDirectory',('"' + $OutputDirectory + '"'))
    Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList $arguments -WorkingDirectory $PSScriptRoot
    Write-Output 'SELFTEST_ELEVATION_REQUESTED'
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$run = Join-Path $OutputDirectory "ttd_$stamp.run"
$etl = Join-Path $OutputDirectory "wpr_$stamp.etl"
$result = Join-Path $OutputDirectory "result_$stamp.json"
$ttd = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\ttd-runtime\TTD.exe'))
$ttdExit = $null
$wprStart = $null
$wprStop = $null

try {
    & $ttd -accepteula -out $run -noUI -launch 'C:\Windows\System32\cmd.exe' /c 'exit 0'
    $ttdExit = $LASTEXITCODE
    & wpr.exe -start GeneralProfile -filemode | Out-Null
    $wprStart = $LASTEXITCODE
    Start-Sleep -Milliseconds 500
    & wpr.exe -stop $etl | Out-Null
    $wprStop = $LASTEXITCODE
    $runFiles = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter "ttd_$stamp*.run" -File)
    $outFiles = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter "ttd_$stamp*.out" -File)
    $ok = $ttdExit -eq 0 -and $wprStart -eq 0 -and $wprStop -eq 0 -and
        $runFiles.Count -gt 0 -and ($runFiles | Measure-Object Length -Sum).Sum -gt 0 -and
        $outFiles.Count -gt 0 -and (Test-Path -LiteralPath $etl)
    [ordered]@{
        passed = $ok
        elevated = $true
        ttd_exit = $ttdExit
        wpr_start_exit = $wprStart
        wpr_stop_exit = $wprStop
        run_files = @($runFiles | Select-Object Name,Length)
        out_files = @($outFiles | Select-Object Name,Length)
        etl = if (Test-Path -LiteralPath $etl) { Get-Item -LiteralPath $etl | Select-Object Name,Length } else { $null }
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $result -Encoding utf8
    if (-not $ok) { throw "TTD/WPR integration self-test failed; inspect $result" }
    Write-Output "FORENSIC_SELFTEST_PASSED=$result"
}
finally {
    $status = (& wpr.exe -status 2>&1 | Out-String)
    if ($status -notmatch 'not recording|is not recording|WPR is not recording') {
        & wpr.exe -cancel 2>&1 | Out-Null
    }
}

