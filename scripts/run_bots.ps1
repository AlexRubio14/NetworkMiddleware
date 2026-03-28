# run_bots.ps1 — Launch N HeadlessBot instances against a running NetServer
#
# Usage (from the folder containing NetServer.exe / HeadlessBot.exe):
#   powershell -ExecutionPolicy Bypass -File run_bots.ps1
#   powershell -ExecutionPolicy Bypass -File run_bots.ps1 -Count 10
#   powershell -ExecutionPolicy Bypass -File run_bots.ps1 -Count 50 -Host 192.168.1.5 -Port 9999
#
# Press Ctrl+C to stop all bots.

param(
    [int]    $Count = 50,
    [string] $ServerHost = "127.0.0.1",
    [int]    $ServerPort = 7777,
    [string] $BotPath = ""
)

$ErrorActionPreference = "Stop"

# Resolve HeadlessBot.exe: explicit param > current directory > script directory
if ($BotPath -ne "" -and (Test-Path $BotPath)) {
    $BotExe = $BotPath
} elseif (Test-Path (Join-Path (Get-Location) "HeadlessBot.exe")) {
    $BotExe = Join-Path (Get-Location) "HeadlessBot.exe"
} else {
    $BotExe = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "HeadlessBot.exe"
}

if (-not (Test-Path $BotExe)) {
    Write-Error "HeadlessBot.exe not found.`nUse -BotPath 'C:\path\to\HeadlessBot.exe' to specify its location."
    exit 1
}

Write-Host "Launching $Count bots → ${ServerHost}:${ServerPort}" -ForegroundColor Cyan
Write-Host "Press Ctrl+C to stop all bots." -ForegroundColor Yellow
Write-Host ("-" * 50)

$env_vars = @{
    SERVER_HOST = $ServerHost
    SERVER_PORT = "$ServerPort"
}

$processes = @()
for ($i = 1; $i -le $Count; $i++) {
    $p = Start-Process -FilePath $BotExe `
                       -PassThru `
                       -WindowStyle Hidden `
                       -Environment $env_vars
    $processes += $p
    Write-Host "  [+] Bot $i  PID=$($p.Id)"
}

Write-Host ("-" * 50)
Write-Host "$Count bots running. Waiting... (Ctrl+C to kill all)" -ForegroundColor Green

try {
    while ($true) { Start-Sleep -Seconds 5 }
} finally {
    Write-Host "`nStopping all bots..." -ForegroundColor Yellow
    foreach ($p in $processes) {
        if (-not $p.HasExited) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host "All bots stopped." -ForegroundColor Green
}
