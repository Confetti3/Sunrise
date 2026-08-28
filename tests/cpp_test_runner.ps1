param(
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [Parameter(Mandatory = $true)]
    [string[]]$Sources
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$include = Join-Path $repo "Sunrise\src"
$temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $temporaryRoot "sunrise-$Name-$PID"))
if (-not $outputDirectory.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The test output directory must remain below the system temporary directory."
}
$objectDirectory = Join-Path $outputDirectory "objects"
$output = Join-Path $outputDirectory "$Name.exe"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe is required to locate the MSVC build environment."
}
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw "A Visual Studio installation with the MSVC x64 tools is required."
}
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat was not found beneath $installation."
}

New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
$quotedSources = ($Sources | ForEach-Object { '"{0}"' -f $_ }) -join ' '
$command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" {3} /Fe:"{4}"' -f $objectDirectory, $vcvars, $include, $quotedSources, $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed to compile."
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed."
    }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
