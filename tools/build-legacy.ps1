# Development reference only. The native application is built with ../build.ps1.
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
& '.\.venv\Scripts\python.exe' -m PyInstaller --noconfirm --clean EDM-Studio.spec
if ($LASTEXITCODE -ne 0) { throw 'Legacy PyInstaller build failed' }
Write-Output 'Legacy binary built under dist; native release aliases were not changed.'
