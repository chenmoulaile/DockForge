# tools/build.ps1 - canonical build entry point for DockForge.
# Wraps the MinGW-w64 + CMake invocation so every session builds identically.
param(
    [ValidateSet('configure', 'build', 'test', 'all')][string]$Action = 'all',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Clean
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

function Invoke-Build {
    & $CMake --build $BuildDir -j 8
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
