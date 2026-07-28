[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$lock = Get-Content -Raw -LiteralPath (
    Join-Path $projectRoot "toolchains.lock.json"
) | ConvertFrom-Json

$toolchainRoot = Join-Path $projectRoot "build\toolchains"
$downloadRoot = Join-Path $toolchainRoot "downloads"
$javaMarker = Join-Path $toolchainRoot ".java-ready"
$gradleMarker = Join-Path $toolchainRoot ".gradle-ready"

New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null

function Get-VerifiedArchive {
    param(
        [string]$Url,
        [string]$ArchiveName,
        [string]$ExpectedSha256
    )

    $archivePath = Join-Path $downloadRoot $ArchiveName
    if (-not (Test-Path -LiteralPath $archivePath)) {
        Write-Host "Downloading $ArchiveName"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $archivePath
    }

    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
    if ($actual.ToLowerInvariant() -ne $ExpectedSha256.ToLowerInvariant()) {
        throw "Checksum mismatch for $archivePath."
    }

    return $archivePath
}

if (-not (Test-Path -LiteralPath $javaMarker)) {
    $javaArchive = Get-VerifiedArchive `
        -Url $lock.java.url `
        -ArchiveName $lock.java.archive `
        -ExpectedSha256 $lock.java.sha256
    Write-Host "Extracting Eclipse Temurin $($lock.java.version)"
    Expand-Archive -LiteralPath $javaArchive -DestinationPath $toolchainRoot
    $javaHome = Get-ChildItem -LiteralPath $toolchainRoot -Directory |
        Where-Object { $_.Name -like "jdk-21*" } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $javaHome -or -not (Test-Path (Join-Path $javaHome "bin\javac.exe"))) {
        throw "The JDK archive did not produce a usable javac."
    }
    $javaHome | Out-File -LiteralPath $javaMarker -Encoding utf8 -NoNewline
}

if (-not (Test-Path -LiteralPath $gradleMarker)) {
    $gradleArchive = Get-VerifiedArchive `
        -Url $lock.gradle.url `
        -ArchiveName $lock.gradle.archive `
        -ExpectedSha256 $lock.gradle.sha256
    Write-Host "Extracting Gradle $($lock.gradle.version)"
    Expand-Archive -LiteralPath $gradleArchive -DestinationPath $toolchainRoot
    $gradleHome = Join-Path $toolchainRoot "gradle-$($lock.gradle.version)"
    if (-not (Test-Path (Join-Path $gradleHome "bin\gradle.bat"))) {
        throw "The Gradle archive did not produce a usable executable."
    }
    $gradleHome | Out-File `
        -LiteralPath $gradleMarker `
        -Encoding utf8 `
        -NoNewline
}

$resolvedJavaHome = Get-Content -Raw -LiteralPath $javaMarker
$resolvedGradleHome = Get-Content -Raw -LiteralPath $gradleMarker

Write-Output "JAVA_HOME=$resolvedJavaHome"
Write-Output "GRADLE_HOME=$resolvedGradleHome"
