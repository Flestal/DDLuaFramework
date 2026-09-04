[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ttd = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\ttd-runtime\TTD.exe'))
if (-not (Test-Path -LiteralPath $ttd)) { throw "TTD.exe not found: $ttd" }

Write-Host 'Microsoft TTD will display its license. Read it and type Y only if you accept it.'
& $ttd -help
if ($LASTEXITCODE -ne 0) {
    throw 'TTD did not confirm license acceptance. No marker was created.'
}

$marker = Join-Path (Split-Path -Parent $ttd) '.eula-accepted-by-user'
Set-Content -LiteralPath $marker -Value (Get-Date).ToString('o') -Encoding ascii
Write-Host "TTD EULA acceptance marker created: $marker"

