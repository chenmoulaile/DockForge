# tools/gate.ps1 - deterministic build+test gate for workflow runs.
# Defaults to ErrorActionPreference Continue so native-command stderr (CMake
# warnings) can never abort the gate; the exit code is the only verdict.
param([ValidateSet('build', 'test', 'all')][string]$Action = 'all')

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$env:PATH = "D:\MSYS2\mingw64\bin;D:\CMake\bin;$env:PATH"

& D:\CMake\bin\cmake.exe --build (Join-Path $Root 'build') -j 8
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($Action -ne 'build') {
    & (Join-Path $Root 'build\bin\dockforge_tests.exe')
    if ($LASTEXITCODE -ne 0) { exit 2 }
}
exit 0
