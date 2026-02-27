# =============================================================================
# setup.ps1 — Bootstrap MS Detours for TimeMockerCpp
#
# Run once before opening the solution in Visual Studio:
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
#
# What it does:
#   1. Clones / updates vcpkg (if not already present at $env:VCPKG_ROOT or ./vcpkg)
#   2. Installs detours:x64-windows and detours:x86-windows
#   3. Copies the resulting headers + libs into packages\detours\
#      so the vcxproj files can find them without requiring vcpkg integration.
# =============================================================================

$ErrorActionPreference = "Stop"

$scriptDir   = $PSScriptRoot
$repoRoot    = Split-Path $scriptDir -Parent
$pkgDir      = Join-Path $repoRoot "packages\detours"
$vcpkgRoot   = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $repoRoot "vcpkg" }

# ── 1. Ensure vcpkg ──────────────────────────────────────────────────────────
if (!(Test-Path (Join-Path $vcpkgRoot "vcpkg.exe")))
{
    Write-Host "Cloning vcpkg into $vcpkgRoot ..." -ForegroundColor Cyan
    git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
    & (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
}
else
{
    Write-Host "vcpkg found at $vcpkgRoot" -ForegroundColor Green
}

$vcpkg = Join-Path $vcpkgRoot "vcpkg.exe"

# ── 2. Install Detours ───────────────────────────────────────────────────────
Write-Host "Installing detours:x64-windows ..." -ForegroundColor Cyan
& $vcpkg install "detours:x64-windows"

Write-Host "Installing detours:x86-windows ..." -ForegroundColor Cyan
& $vcpkg install "detours:x86-windows"

# ── 3. Copy headers + libs into packages\detours\ ───────────────────────────
$x64installed = Join-Path $vcpkgRoot "installed\x64-windows"
$x86installed = Join-Path $vcpkgRoot "installed\x86-windows"

$incSrc = Join-Path $x64installed "include"
$incDst = Join-Path $pkgDir       "include"

Write-Host "Copying headers → $incDst" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $incDst | Out-Null
Copy-Item -Path (Join-Path $incSrc "detours.h") -Destination $incDst -Force

foreach ($triplet in @("x64", "x86"))
{
    $libSrc = Join-Path (Join-Path $vcpkgRoot "installed\$triplet-windows") "lib\detours.lib"
    $libDst = Join-Path $pkgDir "lib\$triplet"
    New-Item -ItemType Directory -Force -Path $libDst | Out-Null
    Copy-Item -Path $libSrc -Destination (Join-Path $libDst "detours.lib") -Force
    Write-Host "Copied $triplet detours.lib → $libDst" -ForegroundColor Green
}

Write-Host ""
Write-Host "Setup complete! Open TimeMocker.sln in Visual Studio 2022." -ForegroundColor Green
Write-Host "Build configuration: Debug|x64 or Release|x64" -ForegroundColor Green
