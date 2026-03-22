# run_tests.ps1 — Build + run all tests on Windows (MSVC)
# Usage: .\scripts\run_tests.ps1 [--rebuild]
# From repo root: powershell -ExecutionPolicy Bypass -File scripts\run_tests.ps1
param(
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "cmake-build-test"

# Find CMake (CLion bundled or system)
$systemCmake = (Get-Command cmake -ErrorAction SilentlyContinue)
$CmakePaths = @(
    "C:\Program Files\JetBrains\CLion 2025.3.3\bin\cmake\win\x64\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    $(if ($systemCmake) { $systemCmake.Source } else { $null })
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

if (-not $CmakePaths) {
    Write-Error "CMake not found. Add CMake to PATH or install CLion."
    exit 1
}
$cmake = $CmakePaths
Write-Host "[cmake] Using: $cmake" -ForegroundColor Cyan

# Configure (only if build dir missing or --rebuild requested)
if ($Rebuild -and (Test-Path $BuildDir)) {
    Write-Host "[cmake] Cleaning build dir..." -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
}

if (-not (Test-Path $BuildDir)) {
    Write-Host "[cmake] Configuring..." -ForegroundColor Cyan
    & $cmake -S $RepoRoot -B $BuildDir -DBUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Build
Write-Host "[cmake] Building MiddlewareTests..." -ForegroundColor Cyan
& $cmake --build $BuildDir --config Debug --target MiddlewareTests
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] Build failed." -ForegroundColor Red
    exit $LASTEXITCODE
}

# Run tests
$TestExe = Join-Path $BuildDir "tests\Debug\MiddlewareTests.exe"
if (-not (Test-Path $TestExe)) {
    Write-Error "Test executable not found at: $TestExe"
    exit 1
}

Write-Host ""
Write-Host "[tests] Running all tests..." -ForegroundColor Cyan
Write-Host ("-" * 60)
& $TestExe --gtest_color=yes
$ExitCode = $LASTEXITCODE
Write-Host ("-" * 60)

if ($ExitCode -eq 0) {
    Write-Host "[PASS] All tests passed." -ForegroundColor Green
} else {
    Write-Host "[FAIL] Some tests failed (exit code $ExitCode)." -ForegroundColor Red
}
exit $ExitCode
