[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

Push-Location $projectRoot
try {
    cmake --preset windows-msvc
    if ($LASTEXITCODE -ne 0) {
        throw "C++ configuration failed."
    }

    cmake --build --preset windows-msvc --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "C++ build failed."
    }

    ctest --preset windows-msvc
    if ($LASTEXITCODE -ne 0) {
        throw "C++ tests failed."
    }

    $engineProbe = Join-Path (
        $projectRoot
    ) "build\windows-msvc\Debug\iramix_engine_probe.exe"
    if (-not (Test-Path -LiteralPath $engineProbe)) {
        throw "Engine probe was not produced."
    }

    powershell.exe `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot "gradle.ps1") `
        check `
        "-PiramixEngineProbe=$engineProbe"
    if ($LASTEXITCODE -ne 0) {
        throw "Java UI or cross-process verification failed."
    }
}
finally {
    Pop-Location
}

