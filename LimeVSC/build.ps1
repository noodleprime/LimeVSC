param(
    [switch]$Test,
    [switch]$Clean,
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
        -latest -property installationPath
if (-not $vs) { throw "Visual Studio not found" }

$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
foreach ($t in @($cmake, $ninja, $vcvars)) {
    if (-not (Test-Path $t)) { throw "missing toolchain component: $t" }
}

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

$envDump = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path (Join-Path $build "build.ninja"))) {
    $cmakeArgs = @(
        "-S", "$root", "-B", "$build", "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_C_COMPILER=cl",
        "-DCMAKE_CXX_COMPILER=cl",
        "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
    )
    & $cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if ($Test) {
    & (Join-Path $build "bin\limetests.exe")
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

Write-Host "`nOK -> $build\bin" -ForegroundColor Green
