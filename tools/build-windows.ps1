param(
  [string]$Version = '0.1.9',
  [string]$BuildDir = 'build-release',
  [string]$CMake = 'C:\Program Files\CMake\bin\cmake.exe',
  [string]$Python = 'python',
  [switch]$NoPackage
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$build = [IO.Path]::GetFullPath($build)
# Windows PowerShell 5.1 wraps redirected native stderr as ErrorRecords.
# CMake warnings must not become terminating errors; check exit codes instead.
$ErrorActionPreference = 'Continue'
& $CMake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DGENESIS_RECOMP_ROOT=$root/segagenesisrecomp" "-DRKA_BUILD_VERSION=$Version" `
  -DSONIC_REVERSE_DEBUG=OFF -DGEN_ENABLE_TRACE=OFF -DGEN_DEV_TRACE=OFF `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE) { throw 'Release configure failed' }
& $CMake --build $build --config Release --target RKARecomp --parallel 8
if ($LASTEXITCODE) { throw 'Release build failed' }
$ErrorActionPreference = 'Stop'
if (-not $NoPackage) {
  & "$PSScriptRoot/make_release.ps1" -Version $Version -BuildDir "$build/Release" -Python $Python
}
