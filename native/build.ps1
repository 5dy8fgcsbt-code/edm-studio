param([string]$Configuration='Release', [string]$Target='')
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools were not found' }
$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build=Join-Path $PSScriptRoot '../build/native'
# Some launcher environments contain both Path and PATH; MSBuild rejects them.
# Normalize only the child process environment, without changing user settings.
function Invoke-CMake([string[]]$Arguments) {
    $info=[System.Diagnostics.ProcessStartInfo]::new($cmake)
    $info.UseShellExecute=$false
    $info.Environment.Clear()
    foreach ($item in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $info.Environment[$item.Key.ToUpperInvariant()]=$item.Value
    }
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process=[System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "CMake failed ($($process.ExitCode))" }
}
Invoke-CMake @('-S',$PSScriptRoot,'-B',$build,'-A','x64')
$buildArguments = @('--build',$build,'--config',$Configuration,'--parallel','8')
if ($Target) { $buildArguments += @('--target',$Target) }
Invoke-CMake $buildArguments


