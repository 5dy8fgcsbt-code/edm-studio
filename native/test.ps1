param([string]$Configuration = 'Release', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ build tools were not found' }
$ctest = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
& $ctest --test-dir (Join-Path $PSScriptRoot '../build/native') -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed ($LASTEXITCODE)" }
