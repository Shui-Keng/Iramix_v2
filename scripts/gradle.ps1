[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GradleArguments
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

powershell.exe `
    -NoProfile `
    -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot "bootstrap-java.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "Java toolchain bootstrap failed."
}

$toolchainRoot = Join-Path $projectRoot "build\toolchains"
$env:JAVA_HOME = Get-Content -Raw -LiteralPath (
    Join-Path $toolchainRoot ".java-ready"
)
$gradleHome = Get-Content -Raw -LiteralPath (
    Join-Path $toolchainRoot ".gradle-ready"
)
$env:PATH = "$(Join-Path $env:JAVA_HOME 'bin');$env:PATH"

& (Join-Path $gradleHome "bin\gradle.bat") `
    --project-dir $projectRoot `
    @GradleArguments
exit $LASTEXITCODE

