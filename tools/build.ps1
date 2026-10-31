# tools/build.ps1 - canonical build entry point for DockForge.
# Wraps the MinGW-w64 + CMake invocation so every session builds identically.
param(
    [ValidateSet('configure', 'build', 'test', 'all')][string]$Action = 'all',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean,
    # Parallel compiles. Each cc1plus peaks around 1.2 GB on this tree (config.cpp and
    # main.cpp are the worst), so the default is derived from free memory instead of a
    # fixed 8: with 8 jobs on a 16 GB machine that already has Windows and a browser
    # resident, the compiler dies with "out of memory allocating" and the failure looks
    # like a code error when it is only a scheduling one.
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

$Root       = Split-Path -Parent $PSScriptRoot
$CMake      = 'D:\CMake\bin\cmake.exe'
$Make       = 'D:\MSYS2\mingw64\bin\mingw32-make.exe'
$Toolchain  = Join-Path $Root 'cmake\toolchain-mingw.cmake'
$BuildDir   = Join-Path $Root 'build'
$env:PATH   = "D:\MSYS2\mingw64\bin;D:\CMake\bin;$env:PATH"

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

function Invoke-Configure {
    & $CMake -S $Root -B $BuildDir -G 'MinGW Makefiles' `
        "-DCMAKE_TOOLCHAIN_FILE=$($Toolchain -replace '\\','/')" `
        "-DCMAKE_MAKE_PROGRAM=$($Make -replace '\\','/')" `
        "-DCMAKE_BUILD_TYPE=$Config"
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
}

function Resolve-Jobs {
    if ($Jobs -gt 0) { return $Jobs }
    $os = Get-CimInstance Win32_OperatingSystem
    $free_gb = $os.FreePhysicalMemory / 1MB
    $cpu = [Environment]::ProcessorCount
    # 1.3 GB per compiler process, rounded down, never more than the CPU count, never
    # less than 1. A machine with 2 GB free gets a serial build rather than a crash.
    $by_memory = [int][math]::Floor($free_gb / 1.3)
    $resolved = [math]::Min($by_memory, $cpu)
    if ($resolved -lt 1) { $resolved = 1 }
    return $resolved
}

function Invoke-Build {
    $jobs = Resolve-Jobs
    Write-Output "building with -j $jobs (free memory adaptive)"
    & $CMake --build $BuildDir -j $jobs
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

function Invoke-Test {
    $exe = Join-Path $BuildDir 'bin\dockforge_tests.exe'
    if (-not (Test-Path $exe)) { throw "test binary missing: $exe" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
}

switch ($Action) {
    'configure' { Invoke-Configure }
    'build'     { Invoke-Build }
    'test'      { Invoke-Test }
    'all'       { Invoke-Configure; Invoke-Build; Invoke-Test }
}

Write-Output "OK: $Action"
