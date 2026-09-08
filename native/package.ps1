param([switch]$SkipBuild, [switch]$Source)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(EDMStudioNative VERSION ([\d.]+)') { throw 'Missing version' }
$version = $Matches[1]
$gitRoot = $projectRoot.Replace('\','/')
$commit = (& git -c "safe.directory=$gitRoot" -C $projectRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source revision' }
$workingChanges = @(& git -c "safe.directory=$gitRoot" -C $projectRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source status' }
if ($Source) {
    if ($workingChanges.Count) { throw 'Commit all source changes before creating the source ZIP.' }
    $headCMake = & git -c "safe.directory=$gitRoot" -C $projectRoot show HEAD:native/CMakeLists.txt
    if ($LASTEXITCODE -ne 0 -or ($headCMake -join "`n") -notmatch "VERSION $([regex]::Escape($version)) LANGUAGES") {
        throw 'HEAD version differs from the requested package version.'
    }
}
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') }
$binaryRoot = Join-Path $projectRoot 'build/native/Release'
foreach ($name in @('EDM-Studio-Native.exe','edm-native-cli.exe')) {
    $binary = Get-Item -LiteralPath (Join-Path $binaryRoot $name)
    if ($binary.VersionInfo.ProductVersion -ne $version) {
        throw "$name does not match version $version. Rebuild before packaging."
    }
}
& (Join-Path $PSScriptRoot 'verify-third-party-sources.ps1') -RequireBuildSource
$packageWork = Join-Path $projectRoot ("build/package-" + $version + '-' + [Guid]::NewGuid().ToString('N'))
$bundle = Join-Path $packageWork 'EDM-Studio'
$installed = Join-Path $projectRoot "dist/$version"
$release = Join-Path $projectRoot 'release'
New-Item -ItemType Directory -Path $bundle,$release,$installed -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $binaryRoot 'EDM-Studio-Native.exe') -Destination (Join-Path $bundle 'EDM-Studio.exe')
Copy-Item -LiteralPath (Join-Path $binaryRoot 'edm-native-cli.exe') -Destination $bundle
@{ version = $version; source_commit = $commit; working_tree_dirty = ($workingChanges.Count -gt 0); built_at_utc = [DateTime]::UtcNow.ToString('o') } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bundle 'release-info.json') -Encoding utf8
foreach ($file in @('README.md','THIRD_PARTY_NOTICES.md','LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $bundle
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $bundle -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'samples') -Destination $bundle -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party_sources') -Destination $bundle -Recurse -Force
$licenseSources = @{
    'imgui' = @('native/vendor/imgui/LICENSE.txt')
    'stb' = @('licenses/stb/LICENSE.txt')
    'proggyclean' = @('licenses/proggyclean/LICENSE.txt')
    'eigen' = @('native/vendor/eigen/COPYING.MPL2','native/vendor/eigen/COPYING.BSD','native/vendor/eigen/COPYING.README','native/vendor/eigen/COPYING.MINPACK','native/vendor/eigen/COPYING.APACHE','native/vendor/eigen/COPYING.GPL','native/vendor/eigen/COPYING.LGPL')
    'json' = @('native/vendor/json/LICENSE.MIT')
    'lua' = @('native/vendor/lua/doc/readme.html')
    'miniz' = @('native/vendor/miniz/LICENSE')
    'directxtex' = @('native/vendor/directxtex/LICENSE')
    'edm-parser' = @('edm_studio/edm/LICENSE')
}
foreach ($name in $licenseSources.Keys) {
    $folder = Join-Path $bundle "licenses/$name"
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    foreach ($file in $licenseSources[$name]) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $folder
    }
}
$archive = Join-Path $release "EDM-Studio-$version-Windows-x64.zip"
Compress-Archive -LiteralPath $bundle -DestinationPath $archive -Force
& (Join-Path $PSScriptRoot 'verify-third-party-sources.ps1') -Archive $archive -ArchivePrefix 'EDM-Studio/'
if ($Source) {
    $sourceArchive = Join-Path $release "EDM-Studio-$version-Source.zip"
    & git -c "safe.directory=$($projectRoot.Replace('\','/'))" -C $projectRoot archive --format=zip --output=$sourceArchive HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source archive failed' }
    & (Join-Path $PSScriptRoot 'verify-third-party-sources.ps1') -Archive $sourceArchive -RequireBuildSource
    Copy-Item -LiteralPath $sourceArchive -Destination (Join-Path $release 'EDM-Studio-Source.zip') -Force
}
Copy-Item -LiteralPath $archive -Destination (Join-Path $release 'EDM-Studio-Windows-x64.zip') -Force
Copy-Item -LiteralPath $bundle -Destination $installed -Recurse -Force
Copy-Item -LiteralPath $archive -Destination (Join-Path (Split-Path -Parent $projectRoot) 'EDM-Studio-Windows-x64.zip') -Force
$hashes = [ordered]@{}
foreach ($file in @($archive, (Join-Path $release 'EDM-Studio-Windows-x64.zip'))) {
    $item = Get-Item -LiteralPath $file
    $hashes[$item.Name] = @{ bytes = $item.Length; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
}
if ($Source) {
    foreach ($file in @($sourceArchive, (Join-Path $release 'EDM-Studio-Source.zip'))) {
        $item = Get-Item -LiteralPath $file
        $hashes[$item.Name] = @{ bytes = $item.Length; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
    }
}
$hashes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $release 'SHA256.json') -Encoding utf8
Get-Item -LiteralPath $archive | Select-Object FullName,Length
Get-FileHash -LiteralPath (Join-Path $bundle 'EDM-Studio.exe'),$archive -Algorithm SHA256
