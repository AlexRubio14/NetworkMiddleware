# NetworkMiddleware Server — Windows installer
# Usage: irm https://raw.githubusercontent.com/AlexRubio14/NetworkMiddleware/main/install.ps1 | iex

$repo     = "AlexRubio14/NetworkMiddleware"
$installDir = "$env:LOCALAPPDATA\NetworkMiddleware"
$desktop    = [Environment]::GetFolderPath("Desktop")

Write-Host "NetworkMiddleware Server — installer" -ForegroundColor Cyan
Write-Host "Fetching latest release..."

$release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
$asset   = $release.assets | Where-Object { $_.name -like "*windows*" } | Select-Object -First 1

if (-not $asset) {
    Write-Error "No Windows release asset found. Check https://github.com/$repo/releases"
    exit 1
}

Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size/1MB, 1)) MB)..."
$zipPath = "$env:TEMP\$($asset.name)"
Invoke-WebRequest $asset.browser_download_url -OutFile $zipPath

Write-Host "Installing to $installDir..."
if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
New-Item -ItemType Directory -Path $installDir | Out-Null
Expand-Archive $zipPath -DestinationPath $installDir -Force
Remove-Item $zipPath

# Desktop shortcut
$shell           = New-Object -ComObject WScript.Shell
$shortcut        = $shell.CreateShortcut("$desktop\NetServer.lnk")
$shortcut.TargetPath       = "$installDir\NetServer.exe"
$shortcut.WorkingDirectory = $installDir
$shortcut.Description      = "NetworkMiddleware Authoritative Server"
$shortcut.Save()

Write-Host ""
Write-Host "Done! Shortcut created on Desktop." -ForegroundColor Green
Write-Host "Version installed: $($release.tag_name)"
