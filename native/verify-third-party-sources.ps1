param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Archive = '',
    [string]$ArchivePrefix = '',
    [switch]$RequireBuildSource
)
$ErrorActionPreference = 'Stop'
$manifestPath = 'third_party_sources/eigen-3.4.0.sha256.json'
$manifestFile = Join-Path $Root $manifestPath
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json -AsHashtable
if ($manifest.component -cne 'Eigen' -or $manifest.version -cne '3.4.0' -or
    $manifest.file_count -ne 1784 -or $manifest.files.Count -ne $manifest.file_count) {
    throw 'Unexpected Eigen source manifest. Update the verifier when changing the dependency.'
}
$directories = @('third_party_sources/eigen-3.4.0')
if ($RequireBuildSource) { $directories += 'native/vendor/eigen' }

function Get-EntryHash($Entry) {
    $stream = $Entry.Open()
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($hash.ComputeHash($stream)).ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}

$zip = $null
try {
    if ($Archive) {
        $zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Archive))
        if ($ArchivePrefix -and -not $ArchivePrefix.EndsWith('/')) { $ArchivePrefix += '/' }
        $entry = $zip.GetEntry($ArchivePrefix + $manifestPath)
        if (-not $entry -or (Get-EntryHash $entry) -ne (Get-FileHash -LiteralPath $manifestFile -Algorithm SHA256).Hash) {
            throw 'Archive Eigen manifest is missing or differs from the checked source manifest.'
        }
    }
    foreach ($directory in $directories) {
        if ($zip) {
            $prefix = $ArchivePrefix + $directory + '/'
            $files = @($zip.Entries | Where-Object {
                $_.FullName.StartsWith($prefix, [StringComparison]::Ordinal) -and -not $_.FullName.EndsWith('/')
            })
        } else {
            $folder = Join-Path $Root $directory
            $files = @(Get-ChildItem -LiteralPath $folder -File -Recurse -Force)
        }
        if ($files.Count -ne $manifest.file_count) {
            throw "Eigen source file count mismatch in ${directory}: $($files.Count) / $($manifest.file_count)"
        }
        foreach ($record in $manifest.files.GetEnumerator()) {
            if ($zip) {
                $entry = $zip.GetEntry($prefix + $record.Key)
                if (-not $entry) { throw "Archive is missing $directory/$($record.Key)" }
                $actual = Get-EntryHash $entry
            } else {
                $actual = (Get-FileHash -LiteralPath (Join-Path $folder $record.Key) -Algorithm SHA256).Hash
            }
            if ($actual -ne $record.Value) { throw "Eigen source hash mismatch: $directory/$($record.Key)" }
        }
    }
    $location = if ($Archive) { $Archive } else { $Root }
    Write-Host "Eigen 3.4.0 verified: $($manifest.file_count) files in each of $($directories.Count) source directories ($location)."
} finally {
    if ($zip) { $zip.Dispose() }
}
