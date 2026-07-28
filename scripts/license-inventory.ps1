# Regenerates the declared-license table for the Java UI dependency set.
#
# Reads a Gradle lockfile, resolves every locked coordinate to its POM in the
# Gradle module cache, and reports the license each artifact declares. Runs
# offline: the cache is populated by any prior `gradle check`.
#
# Declared licenses are what the publisher put in the POM. They do not cover
# code an artifact vendors as a binary; see the redistribution notes in
# docs/phase-0/DEPENDENCIES.md.
#
# Exits non-zero if a coordinate is missing from the cache or declares no
# license, so a new dependency cannot pass unnoticed.

[CmdletBinding()]
param(
    [string] $LockFile = "ui/desktop/gradle-windows-x64.lockfile",
    [string] $CacheRoot = (Join-Path $HOME ".gradle/caches/modules-2/files-2.1")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $LockFile)) {
    Write-Error "Lockfile not found: $LockFile"
    exit 2
}
if (-not (Test-Path $CacheRoot)) {
    Write-Error "Gradle module cache not found: $CacheRoot. Run a build first."
    exit 2
}

$coordinates = Get-Content $LockFile |
    Where-Object { $_ -notmatch '^\s*#' -and $_ -match '=' } |
    ForEach-Object { ($_ -split '=')[0] } |
    Where-Object { $_ -match '^[^:]+:[^:]+:[^:]+$' } |
    Sort-Object -Unique

if ($coordinates.Count -eq 0) {
    Write-Error "No locked coordinates found in $LockFile"
    exit 2
}

$failures = 0
$rows = @()

foreach ($coordinate in $coordinates) {
    $parts = $coordinate -split ':'
    $group = $parts[0]
    $artifact = $parts[1]
    $version = $parts[2]

    $moduleDir = Join-Path $CacheRoot (Join-Path $group (Join-Path $artifact $version))
    $license = $null
    $source = $null

    if (Test-Path $moduleDir) {
        $pom = Get-ChildItem -Path $moduleDir -Recurse -Filter "$artifact-$version.pom" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($pom) {
            $source = "pom"
            try {
                [xml] $document = Get-Content $pom.FullName -Raw
                $names = @($document.project.licenses.license |
                    Where-Object { $_ -ne $null } |
                    ForEach-Object { $_.name })
                if ($names.Count -gt 0) {
                    $license = ($names -join " / ")
                }
            } catch {
                $license = $null
            }
        }
    }

    if (-not $license) {
        $license = "UNDECLARED"
        $failures++
        if (-not $source) { $source = "not-cached" }
    }

    $rows += [pscustomobject] @{
        Coordinate = $coordinate
        License    = $license
        Source     = $source
    }
}

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Output

Write-Output "license_inventory lockfile=$LockFile artifacts=$($rows.Count) undeclared=$failures"

if ($failures -gt 0) {
    Write-Error "$failures artifact(s) declare no license or are absent from the cache."
    exit 1
}
exit 0
