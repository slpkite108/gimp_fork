# Launch ONLY a local GIMP Progressive Fork build. This script does not
# install GIMP, write registry keys, or alter Windows file associations.
#
# Example:
# .\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Cold
# .\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Image -FilePath C:\pictures\photo.png
#
# Use -Baseline to compare the unchanged eager loading path.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDir,

    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [ValidateSet("Cold", "Image", "Project")]
    [string] $Mode = "Cold",

    [string] $FilePath,

    [switch] $Baseline,

    [switch] $NoProfile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$build = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
$exe = (Resolve-Path -LiteralPath $Executable).ProviderPath

# Never launch a system-installed GIMP by mistake. Require a Meson build
# directory and its own executable under that directory.
$mesonFile = Join-Path $build "meson-info\intro-buildoptions.json"
if (-not (Test-Path -LiteralPath $mesonFile -PathType Leaf)) {
    throw "Expected a GIMP Meson build directory with meson-info\intro-buildoptions.json: $build"
}

$buildPrefix = $build.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $exe.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to run an executable outside the selected build directory: $exe"
}

$exeName = [IO.Path]::GetFileName($exe)
if ($exeName -notmatch '^gimp-[0-9]+(\.[0-9]+){1,2}\.exe -and -not [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Cold mode accepts no FilePath. Use Image or Project mode."
}
if ($Mode -ne "Cold" -and [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Image and Project modes require -FilePath."
}

$launchArgs = @("--new-instance", "--no-splash")
if ($Mode -ne "Cold") {
    $inputFile = (Resolve-Path -LiteralPath $FilePath).ProviderPath
    if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf)) {
        throw "Input is not a regular file: $inputFile"
    }
    $launchArgs += $inputFile
}

# Save and restore the invoking PowerShell environment. The compiled fork
# forces GIMP3_DIRECTORY, GIMP3_CACHEDIR and GIMP3_TEMPDIR to private
# GIMP-Progressive-Fork locations before libgimpbase is initialized.
$original = @{}
foreach ($key in @("GIMP_PROGRESSIVE_STARTUP", "GIMP_STARTUP_PROFILE")) {
    $original[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
}

try {
    if ($Baseline) {
        $env:GIMP_PROGRESSIVE_STARTUP = "0"
    } else {
        $env:GIMP_PROGRESSIVE_STARTUP = "1"
    }

    if ($NoProfile) {
        $env:GIMP_STARTUP_PROFILE = "0"
    } else {
        $env:GIMP_STARTUP_PROFILE = "1"
    }

    Write-Host "Launching isolated fork: $exe"
    Write-Host "Mode: $Mode  Baseline: $([bool]$Baseline)"
    Write-Host "No installed GIMP process will be contacted; this run uses --new-instance."
    & $exe @launchArgs

    if ($LASTEXITCODE -ne 0) {
        throw "Fork executable exited with code $LASTEXITCODE"
    }
}
finally {
    foreach ($key in $original.Keys) {
        [Environment]::SetEnvironmentVariable($key, $original[$key], "Process")
    }
}
) {
    throw "Refusing to launch unexpected executable (expected the built gimp-X.Y.exe): $exeName"
}

# A build directory can contain an unmodified GIMP binary too. Reject it
# unless the compiled EXE contains the fork's private-profile marker.
# This is a safety check for accidental mis-selection, not a signature.
$binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
if (-not $binaryText.Contains("GIMP_PROGRESSIVE_FORK_ISOLATED") -or
    -not $binaryText.Contains("GIMP-Progressive-Fork")) {
    throw "Executable does not contain the fork isolation markers. Rebuild feature/progressive-startup before launching."
}
Remove-Variable binaryText

if ($Mode -eq "Cold" -and -not [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Cold mode accepts no FilePath. Use Image or Project mode."
}
if ($Mode -ne "Cold" -and [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Image and Project modes require -FilePath."
}

$launchArgs = @("--new-instance", "--no-splash")
if ($Mode -ne "Cold") {
    $inputFile = (Resolve-Path -LiteralPath $FilePath).ProviderPath
    if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf)) {
        throw "Input is not a regular file: $inputFile"
    }
    $launchArgs += $inputFile
}

# Save and restore the invoking PowerShell environment. The compiled fork
# forces GIMP3_DIRECTORY, GIMP3_CACHEDIR and GIMP3_TEMPDIR to private
# GIMP-Progressive-Fork locations before libgimpbase is initialized.
$original = @{}
foreach ($key in @("GIMP_PROGRESSIVE_STARTUP", "GIMP_STARTUP_PROFILE")) {
    $original[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
}

try {
    if ($Baseline) {
        $env:GIMP_PROGRESSIVE_STARTUP = "0"
    } else {
        $env:GIMP_PROGRESSIVE_STARTUP = "1"
    }

    if ($NoProfile) {
        $env:GIMP_STARTUP_PROFILE = "0"
    } else {
        $env:GIMP_STARTUP_PROFILE = "1"
    }

    Write-Host "Launching isolated fork: $exe"
    Write-Host "Mode: $Mode  Baseline: $([bool]$Baseline)"
    Write-Host "No installed GIMP process will be contacted; this run uses --new-instance."
    & $exe @launchArgs

    if ($LASTEXITCODE -ne 0) {
        throw "Fork executable exited with code $LASTEXITCODE"
    }
}
finally {
    foreach ($key in $original.Keys) {
        [Environment]::SetEnvironmentVariable($key, $original[$key], "Process")
    }
}
