param([switch]$Test, [string]$ToolchainPrefix = 'C:/msys64/ucrt64')
$ErrorActionPreference = 'Stop'
if (Test-Path "$ToolchainPrefix/bin") { $env:PATH = "$ToolchainPrefix/bin;" + $env:PATH }
cmake -S $PSScriptRoot -B "$PSScriptRoot/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$ToolchainPrefix" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$PSScriptRoot/build" -j 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Test) { ctest --test-dir "$PSScriptRoot/build" --output-on-failure; exit $LASTEXITCODE }
