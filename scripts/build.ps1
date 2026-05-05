# patX Windows Build Script
#
# Builds the Rust library and the C++ wxWidgets application.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64",

    [switch]$Clean,

    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  patX Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

function Test-Tool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$InstallHint
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        Write-Host "[ERROR] $Name not found. $InstallHint" -ForegroundColor Red
        exit 1
    }

    return $command
}

function Check-Dependencies {
    Write-Host "Checking dependencies..." -ForegroundColor Yellow

    Test-Tool "rustc" "Install Rust from https://rustup.rs" | Out-Null
    $rustVersion = (& rustc --version).Trim()
    Write-Host "[OK] Rust: $rustVersion" -ForegroundColor Green

    Test-Tool "cargo" "Cargo is included with rustup." | Out-Null
    $cargoVersion = (& cargo --version).Trim()
    Write-Host "[OK] Cargo: $cargoVersion" -ForegroundColor Green

    Test-Tool "cmake" "Install CMake from https://cmake.org/download/" | Out-Null
    $cmakeVersion = (& cmake --version | Select-Object -First 1).Trim()
    Write-Host "[OK] CMake: $cmakeVersion" -ForegroundColor Green

    $compiler = Get-Command cl -ErrorAction SilentlyContinue
    if ($compiler) {
        Write-Host "[OK] MSVC compiler is available" -ForegroundColor Green
    } else {
        $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $vsPath = & $vswhere -latest -property installationPath
            Write-Host "[OK] Visual Studio found: $vsPath" -ForegroundColor Green
        } else {
            Write-Host "[WARN] MSVC cl.exe and vswhere were not found in PATH; CMake may still find another generator." -ForegroundColor Yellow
        }
    }

    Write-Host ""
}

function Clean-Build {
    if (-not $Clean) {
        return
    }

    Write-Host "Cleaning build directories..." -ForegroundColor Yellow

    $buildDir = Join-Path $ProjectRoot "build"
    $targetDir = Join-Path $ProjectRoot "target"

    foreach ($dir in @($buildDir, $targetDir)) {
        $resolvedRoot = (Resolve-Path $ProjectRoot).Path
        $resolvedTarget = if (Test-Path $dir) { (Resolve-Path $dir).Path } else { $dir }

        if ((Test-Path $dir) -and $resolvedTarget.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $dir -Recurse -Force
            Write-Host "[OK] Removed: $dir" -ForegroundColor Green
        }
    }

    Write-Host ""
}

function Build-Rust {
    Write-Host "Building Rust library ($Configuration)..." -ForegroundColor Yellow

    Push-Location $ProjectRoot
    try {
        $cargoArgs = @("build")
        if ($Configuration -eq "Release") {
            $cargoArgs += "--release"
        }

        & cargo @cargoArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Rust build failed"
        }
    } finally {
        Pop-Location
    }

    $profileDir = if ($Configuration -eq "Release") { "release" } else { "debug" }
    $rustLib = Join-Path $ProjectRoot "target\$profileDir\patx_core.lib"
    if (Test-Path $rustLib) {
        Write-Host "[OK] Rust library: $rustLib" -ForegroundColor Green
    } else {
        Write-Host "[WARN] Static Rust library was not found at $rustLib" -ForegroundColor Yellow
    }

    Write-Host ""
}

function Build-Cpp {
    Write-Host "Building C++ application..." -ForegroundColor Yellow

    $buildDir = Join-Path $ProjectRoot "build"
    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    $vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" }
    $vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $cmakeArgs = @(
        "-B", $buildDir,
        "-S", $ProjectRoot,
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
    )

    if (Test-Path $vcpkgToolchain) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    }

    if (-not (Test-Path $cacheFile)) {
        if ($Platform -eq "x64") {
            $cmakeArgs += @("-A", "x64")
        } elseif ($Platform -eq "x86") {
            $cmakeArgs += @("-A", "Win32")
        }
    } else {
        Write-Host "[INFO] Reusing existing CMake cache in $buildDir" -ForegroundColor DarkGray
    }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }

    & cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "C++ build failed"
    }

    $exe = Join-Path $buildDir "$Configuration\patx.exe"
    if (Test-Path $exe) {
        Write-Host "[OK] Application: $exe" -ForegroundColor Green
    }

    Write-Host ""
}

function Invoke-ProjectTests {
    if (-not $RunTests) {
        return
    }

    Write-Host "Running tests..." -ForegroundColor Yellow

    Push-Location $ProjectRoot
    try {
        $cargoTestArgs = @("test")
        if ($Configuration -eq "Release") {
            $cargoTestArgs += "--release"
        }

        & cargo @cargoTestArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Rust tests failed"
        }
    } finally {
        Pop-Location
    }

    Write-Host ""
}

function Show-Summary {
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Build Summary" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan

    $buildDir = Join-Path $ProjectRoot "build"
    $exe = Join-Path $buildDir "$Configuration\patx.exe"

    if (Test-Path $exe) {
        $sizeKb = [Math]::Round((Get-Item $exe).Length / 1KB, 1)
        Write-Host "Application: $exe ($sizeKb KB)" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "Build completed successfully." -ForegroundColor Green
}

Check-Dependencies
Clean-Build
Build-Rust
Build-Cpp
Invoke-ProjectTests
Show-Summary
