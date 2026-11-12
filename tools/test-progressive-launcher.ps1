# Run with PowerShell; no GIMP installation or compiled fork is needed.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$launcher = Join-Path $PSScriptRoot 'run-progressive-isolated.ps1'
$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    $launcher, [ref]$tokens, [ref]$parseErrors) | Out-Null
if ($parseErrors.Count -ne 0) {
    throw "Launcher does not parse: $($parseErrors.Message -join '; ')"
}

function Assert-Rejected([hashtable] $Arguments, [string] $Expected) {
    try {
        & $launcher @Arguments
    } catch {
        if (-not $_.Exception.Message.Contains($Expected)) {
            throw "Expected '$Expected', received '$($_.Exception.Message)'"
        }
        Write-Output "PASS: $Expected"
        return
    }
    throw "Launcher accepted input that should have been rejected: $Expected"
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $tempRoot ('gimp-launcher-test-' + [guid]::NewGuid())
$savedEnvironment = @{}
foreach ($name in @('GIMP_PROGRESSIVE_STARTUP', 'GIMP_STARTUP_PROFILE', 'GIMP_LAUNCHER_TEST_RESULT')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $build = Join-Path $fixture 'build'
    $app = Join-Path $build 'app'
    New-Item -ItemType Directory -Path $app -Force | Out-Null
    $exe = Join-Path $app 'gimp-3.3.exe'
    [IO.File]::WriteAllText($exe, 'unmodified GIMP fixture; never executable')
    Assert-Rejected @{BuildDir=$build; Executable=$exe} 'Expected a GIMP Meson build directory'

    $info = Join-Path $build 'meson-info'
    New-Item -ItemType Directory -Path $info | Out-Null
    [IO.File]::WriteAllText((Join-Path $info 'intro-buildoptions.json'), '[]')
    $outside = Join-Path $fixture 'gimp-3.3.exe'
    Copy-Item -LiteralPath $exe -Destination $outside
    Assert-Rejected @{BuildDir=$build; Executable=$outside} 'outside the selected build directory'
    Assert-Rejected @{BuildDir=$build; Executable=$exe} 'does not contain the fork isolation markers'

    $wrongName = Join-Path $app 'other.exe'
    Copy-Item -LiteralPath $exe -Destination $wrongName
    Assert-Rejected @{BuildDir=$build; Executable=$wrongName} 'unexpected executable'

    [IO.File]::WriteAllText($exe, 'GIMP_PROGRESSIVE_FORK_ISOLATED GIMP-Progressive-Fork')
    Assert-Rejected @{BuildDir=$build; Executable=$exe; Mode='Cold'; FilePath='sample.png'} 'Cold mode accepts no FilePath'
    foreach ($mode in @('Image', 'Project')) {
        Assert-Rejected @{BuildDir=$build; Executable=$exe; Mode=$mode} 'Image and Project modes require -FilePath'
    }

    # Exercise the Windows GUI process lifecycle, including a nonzero exit.
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
    $source = Join-Path $fixture 'GuiFixture.cs'
    [IO.File]::WriteAllText($source, @'
using System;
using System.IO;
using System.Threading;
class GuiFixture {
    static int Main(string[] args) {
        Thread.Sleep(200);
        File.WriteAllLines(Environment.GetEnvironmentVariable("GIMP_LAUNCHER_TEST_RESULT"),
            new[] { String.Join("|", args),
                    Environment.GetEnvironmentVariable("GIMP_PROGRESSIVE_STARTUP"),
                    Environment.GetEnvironmentVariable("GIMP_STARTUP_PROFILE") });
        return 7;
    }
}
'@)
    & $compiler /nologo /target:winexe "/out:$exe" $source
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the GUI launcher fixture' }
    [IO.File]::AppendAllText($exe, 'GIMP_PROGRESSIVE_FORK_ISOLATED GIMP-Progressive-Fork')
    $sample = Join-Path $fixture 'sample image.png'
    [IO.File]::WriteAllText($sample, 'argument fixture')
    $env:GIMP_LAUNCHER_TEST_RESULT = Join-Path $fixture 'child-result.txt'
    $env:GIMP_PROGRESSIVE_STARTUP = 'original-startup'
    $env:GIMP_STARTUP_PROFILE = 'original-profile'
    Assert-Rejected @{BuildDir=$build; Executable=$exe; Mode='Image'; FilePath=$sample; Baseline=$true; NoProfile=$true} 'Fork executable exited with code 7'
    $result = [IO.File]::ReadAllLines($env:GIMP_LAUNCHER_TEST_RESULT)
    if ($result[0] -ne "--new-instance|--no-splash|$sample" -or $result[1] -ne '0' -or $result[2] -ne '0') {
        throw 'Child arguments or startup flags were not preserved'
    }
    if ($env:GIMP_PROGRESSIVE_STARTUP -ne 'original-startup' -or $env:GIMP_STARTUP_PROFILE -ne 'original-profile') {
        throw 'Launcher did not restore its caller environment after child failure'
    }
    Write-Output 'PASS: GUI completion, spaced filename, startup flags and environment restoration'
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    $target = [IO.Path]::GetFullPath($fixture)
    $prefix = $tempRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Test cleanup escaped its temporary directory: $target"
    }
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}
