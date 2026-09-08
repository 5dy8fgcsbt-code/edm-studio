$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$vendor = Join-Path $taskRoot 'vendor'
$cache = Join-Path $taskRoot '../build/native-downloads'
New-Item -ItemType Directory -Force -Path $vendor,$cache | Out-Null
function Fetch-Zip([string]$Name,[string]$Url) {
    $target = Join-Path $vendor $Name
    if (Test-Path $target) { return }
    $archive = Join-Path $cache ($Name+'.zip')
    Invoke-WebRequest -Uri $Url -OutFile $archive
    $unpack = Join-Path $cache ($Name+'-unpack')
    Expand-Archive -LiteralPath $archive -DestinationPath $unpack -Force
    $source = Get-ChildItem -LiteralPath $unpack -Directory | Select-Object -First 1
    Copy-Item -LiteralPath $source.FullName -Destination $target -Recurse
}
Fetch-Zip 'imgui' 'https://github.com/ocornut/imgui/archive/refs/tags/v1.92.5.zip'
Fetch-Zip 'json' 'https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip'
Fetch-Zip 'miniz' 'https://github.com/richgel999/miniz/archive/refs/tags/3.1.0.zip'
Fetch-Zip 'eigen' 'https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip'
Fetch-Zip 'directxtex' 'https://github.com/microsoft/DirectXTex/archive/refs/tags/oct2025.zip'
# Independent FBX validation only; not an application runtime dependency.
$ufbx = Join-Path $vendor 'ufbx'
New-Item -ItemType Directory -Force -Path $ufbx | Out-Null
foreach ($file in @('ufbx.c','ufbx.h','LICENSE')) {
    $destination = Join-Path $ufbx $file
    if (-not (Test-Path -LiteralPath $destination)) {
        Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/ufbx/ufbx/fcc5d6ba444cfd3eb80677dba5e37e493941abe5/' + $file) -OutFile $destination
    }
}
if (-not (Test-Path (Join-Path $vendor 'lua'))) {
    $archive = Join-Path $cache 'lua.tar.gz'
    Invoke-WebRequest -Uri 'https://www.lua.org/ftp/lua-5.4.8.tar.gz' -OutFile $archive
    & tar -xzf $archive -C $cache
    if ($LASTEXITCODE -ne 0) { throw 'Lua archive extraction failed' }
    Copy-Item -LiteralPath (Join-Path $cache 'lua-5.4.8') -Destination (Join-Path $vendor 'lua') -Recurse
}
Get-ChildItem -LiteralPath $vendor -Directory | Select-Object Name
