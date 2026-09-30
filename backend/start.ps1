param([string]$ToolchainPrefix = 'C:/msys64/ucrt64')
$ErrorActionPreference = 'Stop'
if (Test-Path "$ToolchainPrefix/bin") { $env:PATH = "$ToolchainPrefix/bin;" + $env:PATH }
$executable = Join-Path $PSScriptRoot 'build/tradermade_backend.exe'
if (!(Test-Path $executable)) { throw 'Build the backend first: powershell -ExecutionPolicy Bypass -File backend/build.ps1 -Test' }
& $executable --root (Split-Path $PSScriptRoot)
exit $LASTEXITCODE
