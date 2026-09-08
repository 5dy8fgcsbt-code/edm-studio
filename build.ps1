param([switch]$Source)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'native/build.ps1')
& (Join-Path $PSScriptRoot 'native/test.ps1') -SkipBuild
& (Join-Path $PSScriptRoot 'native/package.ps1') -SkipBuild -Source:$Source
