# =============================================================================
# setup.ps1 — Bootstrap MS Detours + Dear ImGui for TimeMockerCpp
#
# Run once before opening the solution:
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
# =============================================================================

$ErrorActionPreference = "Stop"

$repoRoot   = Split-Path $PSScriptRoot -Parent
$vcpkgRoot  = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $repoRoot "vcpkg" }
$pkgDir     = Join-Path $repoRoot "packages\detours"
$imguiDir   = Join-Path $repoRoot "TimeMocker.UI\imgui"

# ── 1. vcpkg ─────────────────────────────────────────────────────────────────
if (!(Test-Path (Join-Path $vcpkgRoot "vcpkg.exe")))
{
    Write-Host "Cloning vcpkg..." -ForegroundColor Cyan
    git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
    & (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
}
else { Write-Host "vcpkg found at $vcpkgRoot" -ForegroundColor Green }

$vcpkg = Join-Path $vcpkgRoot "vcpkg.exe"

# ── 2. Install Detours ────────────────────────────────────────────────────────
Write-Host "Installing detours:x64-windows..." -ForegroundColor Cyan
& $vcpkg install "detours:x64-windows"
Write-Host "Installing detours:x86-windows..." -ForegroundColor Cyan
& $vcpkg install "detours:x86-windows"

# ── 3. Copy Detours headers + libs ───────────────────────────────────────────
$incDst = Join-Path $pkgDir "include"
New-Item -ItemType Directory -Force -Path $incDst | Out-Null
Copy-Item -Path (Join-Path $vcpkgRoot "installed\x64-windows\include\detours.h") `
          -Destination $incDst -Force

foreach ($triplet in @("x64","x86"))
{
    $libDst = Join-Path $pkgDir "lib\$triplet"
    New-Item -ItemType Directory -Force -Path $libDst | Out-Null
    Copy-Item -Path (Join-Path $vcpkgRoot "installed\$triplet-windows\lib\detours.lib") `
              -Destination (Join-Path $libDst "detours.lib") -Force
    Write-Host "  Detours $triplet copied" -ForegroundColor Green
}

# ── 4. Download Dear ImGui ────────────────────────────────────────────────────
Write-Host ""
Write-Host "Fetching Dear ImGui (latest release)..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $imguiDir | Out-Null

# Use the GitHub API to find the latest release tag
$release = Invoke-RestMethod "https://api.github.com/repos/ocornut/imgui/releases/latest"
$tag = $release.tag_name
Write-Host "  Tag: $tag" -ForegroundColor Green

$baseUrl = "https://raw.githubusercontent.com/ocornut/imgui/$tag"

$coreFiles = @(
    "imgui.h", "imgui.cpp",
    "imgui_internal.h",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "imconfig.h",
    "imstb_rectpack.h",
    "imstb_textedit.h",
    "imstb_truetype.h"
)

$backendFiles = @(
    "imgui_impl_win32.h", "imgui_impl_win32.cpp",
    "imgui_impl_dx11.h",  "imgui_impl_dx11.cpp"
)

foreach ($f in $coreFiles)
{
    $url  = "$baseUrl/$f"
    $dest = Join-Path $imguiDir $f
    Write-Host "  Downloading $f" -NoNewline
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    Write-Host " ✓" -ForegroundColor Green
}

foreach ($f in $backendFiles)
{
    $url  = "$baseUrl/backends/$f"
    $dest = Join-Path $imguiDir $f
    Write-Host "  Downloading $f" -NoNewline
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    Write-Host " ✓" -ForegroundColor Green
}

Write-Host ""
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host " Setup complete!" -ForegroundColor Green
Write-Host " Open TimeMocker.sln in Visual Studio 2022" -ForegroundColor Green
Write-Host " Build: Release | x64" -ForegroundColor Green
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
