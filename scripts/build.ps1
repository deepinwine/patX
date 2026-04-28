# patX Windows Build Script
#
# 编译 C++/Rust 混合项目

param(
    [string] = "Release",
    [string] = "x64",
    [bool] = False,
    [bool] = False
)

Continue = "Stop"
 =  | Split-Path -Parent

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  patX Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# 检查依赖
function CheckDependencies {
    Write-Host "Checking dependencies..." -ForegroundColor Yellow
    
    # 检查 Rust
     = Get-Command rustc -ErrorAction SilentlyContinue
    if (-not ) {
        Write-Host "[ERROR] Rust not found. Please install via:" -ForegroundColor Red
        Write-Host "        https://rustup.rs" -ForegroundColor Red
        exit 1
    }
    
     = (rustc --version).ToString()
    Write-Host "[OK] Rust: " -ForegroundColor Green
    
    # 检查 Cargo
     = Get-Command cargo -ErrorAction SilentlyContinue
    if (-not ) {
        Write-Host "[ERROR] Cargo not found" -ForegroundColor Red
        exit 1
    }
    Write-Host "[OK] Cargo: " -ForegroundColor Green
    
    # 检查 CMake
     = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not ) {
        Write-Host "[ERROR] CMake not found. Please install via:" -ForegroundColor Red
        Write-Host "        https://cmake.org/download/" -ForegroundColor Red
        exit 1
    }
    Write-Host "[OK] CMake: " -ForegroundColor Green
    
    # 检查 C++ 编译器 (MSVC)
     = Get-Command cl -ErrorAction SilentlyContinue
    if (-not ) {
        Write-Host "[WARN] MSVC cl.exe not in PATH" -ForegroundColor Yellow
        Write-Host "       Will try to find via vswhere" -ForegroundColor Yellow
        
        # 尝试通过 vswhere 找到 MSVC
         = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path ) {
             = &  -latest -property installationPath
            Write-Host "[OK] Visual Studio: " -ForegroundColor Green
        } else {
            Write-Host "[WARN] vswhere not found, assuming CMake will find compiler" -ForegroundColor Yellow
        }
    } else {
        Write-Host "[OK] MSVC Compiler available" -ForegroundColor Green
    }
    
    Write-Host ""
}

# 清理构建目录
function CleanBuild {
    if () {
        Write-Host "Cleaning build directories..." -ForegroundColor Yellow
        
         = Join-Path  "build"
         = Join-Path  "target"
        
        if (Test-Path ) {
            Remove-Item -Path  -Recurse -Force
            Write-Host "[OK] Removed: " -ForegroundColor Green
        }
        
        if (Test-Path ) {
            Remove-Item -Path  -Recurse -Force
            Write-Host "[OK] Removed: " -ForegroundColor Green
        }
        
        Write-Host ""
    }
}

# 构建 Rust 库
function BuildRust {
    Write-Host "Building Rust library ()..." -ForegroundColor Yellow
    
    Push-Location 
    
     = @("build")
    if ( -eq "Release") {
         += "--release"
    }
    
    & cargo 
    
    if (1 -ne 0) {
        Write-Host "[ERROR] Rust build failed" -ForegroundColor Red
        Pop-Location
        exit 1
    }
    
    Pop-Location
    
    # 确认输出
     = Join-Path  "target"
     = if ( -eq "Release") {
        Join-Path  "release\patx_core.lib"
    } else {
        Join-Path  "debug\patx_core.lib"
    }
    
    if (Test-Path ) {
        Write-Host "[OK] Rust library: " -ForegroundColor Green
    } else {
        Write-Host "[WARN] Static lib not found (may be cdylib only)" -ForegroundColor Yellow
    }
    
    Write-Host ""
}

# 构建 C++ 库和主程序
function BuildCpp {
    Write-Host "Building C++ library and main program..." -ForegroundColor Yellow
    
     = Join-Path  "build"
    
    # CMake 配置
     = @(
        "-B", ,
        "-S", ,
        "-DCMAKE_BUILD_TYPE="
    )
    
    # 指定生成器 (Windows 默认使用 MSVC)
    if ( -eq "x64") {
         += "-A", "x64"
    } elseif ( -eq "x86") {
         += "-A", "Win32"
    }
    
    & cmake 
    
    if (1 -ne 0) {
        Write-Host "[ERROR] CMake configuration failed" -ForegroundColor Red
        exit 1
    }
    
    # CMake 构建
     = @(
        "--build", ,
        "--config", 
    )
    
    & cmake 
    
    if (1 -ne 0) {
        Write-Host "[ERROR] C++ build failed" -ForegroundColor Red
        exit 1
    }
    
    # 确认输出
     = Join-Path  "\patx.exe"
    if (Test-Path ) {
        Write-Host "[OK] Main executable: " -ForegroundColor Green
    }
    
     = Join-Path  "\patx_test.exe"
    if (Test-Path ) {
        Write-Host "[OK] Test executable: " -ForegroundColor Green
    }
    
     = Join-Path  "\patx_benchmark.exe"
    if (Test-Path ) {
        Write-Host "[OK] Benchmark executable: " -ForegroundColor Green
    }
    
    Write-Host ""
}

# 运行测试
function RunTests {
    if () {
        Write-Host "Running tests..." -ForegroundColor Yellow
        
        # Rust 测试
        Push-Location 
        & cargo test --release
        Pop-Location
        
        Write-Host ""
        
        # C++ 测试
         = Join-Path  "build"
         = Join-Path  "\patx_test.exe"
        
        if (Test-Path ) {
            Write-Host "Running C++ tests..." -ForegroundColor Yellow
            & 
        } else {
            Write-Host "[WARN] C++ test executable not found" -ForegroundColor Yellow
        }
        
        Write-Host ""
    }
}

# 显示构建摘要
function ShowSummary {
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Build Summary" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    
     = Join-Path  "build"
    
    # Rust 输出
     = Join-Path  "target\release\patx_core.dll"
    if (Test-Path ) {
         = (Get-Item ).Length / 1KB
        Write-Host "Rust Library:   (0 KB)" -ForegroundColor Green
    }
    
    # C++ 输出
     = Join-Path  "\patx.exe"
    if (Test-Path ) {
         = (Get-Item ).Length / 1KB
        Write-Host "Main Program:   (0 KB)" -ForegroundColor Green
    }
    
    Write-Host ""
    Write-Host "Build completed successfully!" -ForegroundColor Green
    Write-Host ""
}

# 主流程
CheckDependencies
CleanBuild
BuildRust
BuildCpp
RunTests
ShowSummary
