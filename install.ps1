# NetworkMiddleware Server — Windows installer
# Usage: irm https://raw.githubusercontent.com/AlexRubio14/NetworkMiddleware/main/install.ps1 | iex

$ErrorActionPreference = 'Stop'

$repo       = "AlexRubio14/NetworkMiddleware"
$installDir = "$env:LOCALAPPDATA\NetworkMiddleware"
$desktop    = [Environment]::GetFolderPath("Desktop")
$zipPath    = "$env:TEMP\NetServer-windows-x64.zip"

Write-Host "NetworkMiddleware Server — installer" -ForegroundColor Cyan

# ── Fetch latest release ────────────────────────────────────────────────────
Write-Host "Fetching latest release..."
try {
    $release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
} catch {
    Write-Error "Could not reach GitHub API: $_`nCheck your internet connection or try again later."
    exit 1
}

$asset = $release.assets | Where-Object { $_.name -like "*windows*" } | Select-Object -First 1
if (-not $asset) {
    Write-Error "No Windows release asset found in release '$($release.tag_name)'.`nCheck: https://github.com/$repo/releases"
    exit 1
}

# ── Download ────────────────────────────────────────────────────────────────
Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size/1MB, 1)) MB)..."
try {
    Invoke-WebRequest $asset.browser_download_url -OutFile $zipPath
} catch {
    Write-Error "Download failed: $_"
    exit 1
}

# ── Install (extract to staging dir, validate, then atomic replace) ─────────
$stagingDir = "$installDir.new"
Write-Host "Installing to $installDir..."
try {
    if (Test-Path $stagingDir) { Remove-Item $stagingDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stagingDir | Out-Null
    Expand-Archive $zipPath -DestinationPath $stagingDir -Force
} catch {
    if (Test-Path $stagingDir) { Remove-Item $stagingDir -Recurse -Force }
    Write-Error "Extraction failed: $_"
    exit 1
} finally {
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
}

# ── Validate staging dir before touching the live install ───────────────────
$exe = "$stagingDir\NetServer.exe"
if (-not (Test-Path $exe)) {
    Remove-Item $stagingDir -Recurse -Force
    Write-Error "NetServer.exe not found after extraction. The release package may be corrupted."
    exit 1
}

# ── Atomic swap: only remove old install once new one is verified ────────────
if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
Rename-Item $stagingDir $installDir
$exe = "$installDir\NetServer.exe"

# ── Desktop shortcut ────────────────────────────────────────────────────────
$shell                     = New-Object -ComObject WScript.Shell
$shortcut                  = $shell.CreateShortcut("$desktop\NetServer.lnk")
$shortcut.TargetPath       = $exe
$shortcut.WorkingDirectory = $installDir
$shortcut.Description      = "NetworkMiddleware Authoritative Server"
$shortcut.Save()

Write-Host ""
Write-Host "Done! Shortcut created on Desktop." -ForegroundColor Green
Write-Host "Version installed: $($release.tag_name)"
