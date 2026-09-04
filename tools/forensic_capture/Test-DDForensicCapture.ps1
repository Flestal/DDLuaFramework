[CmdletBinding()]
param(
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\DarkestDungeon',
    [string]$OutputRoot = 'D:\Documents\코덱스\DDMOD\DDLuaFramework\forensic_captures',
    [int]$MinimumFreeGiB = 100
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$failures = [Collections.Generic.List[string]]::new()
$ttd = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\ttd-runtime\TTD.exe'))
$game = Join-Path $GameRoot '_windows\win64\Darkest.exe'
$marker = Join-Path (Split-Path -Parent $ttd) '.eula-accepted-by-user'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path -LiteralPath $ttd)) { $failures.Add("Missing TTD: $ttd") }
if (-not (Test-Path -LiteralPath $game)) { $failures.Add("Missing game: $game") }
foreach ($command in 'wpr.exe','robocopy.exe') {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { $failures.Add("Missing command: $command") }
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { $failures.Add('Missing command: python') }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$driveName = ([IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $OutputRoot).Path)).TrimEnd('\').TrimEnd(':')
$freeGiB = [math]::Floor((Get-PSDrive -Name $driveName).Free / 1GB)
if ($freeGiB -lt $MinimumFreeGiB) { $failures.Add("Only $freeGiB GiB free; $MinimumFreeGiB GiB required") }
if (-not (Test-Path -LiteralPath $marker)) { $failures.Add('TTD EULA acceptance marker is missing') }
if (-not $isAdministrator) { $failures.Add('Current PowerShell is not elevated; recording will request UAC elevation') }

$result = [ordered]@{
    ready = $failures.Count -eq 0
    ttd = $ttd
    ttd_sha256 = if (Test-Path -LiteralPath $ttd) { (Get-FileHash -Algorithm SHA256 -LiteralPath $ttd).Hash } else { $null }
    game = $game
    game_sha256 = if (Test-Path -LiteralPath $game) { (Get-FileHash -Algorithm SHA256 -LiteralPath $game).Hash } else { $null }
    native_decoder = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\decode_native_trace.py'))
    output_root = (Resolve-Path -LiteralPath $OutputRoot).Path
    free_gib = $freeGiB
    administrator = $isAdministrator
    failures = @($failures)
}
$result | ConvertTo-Json -Depth 5
if ($failures.Count) { exit 1 }
