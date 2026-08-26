$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$testSource = Join-Path $PSScriptRoot "statics_footprint_cache_test.cpp"
$cacheSource = Join-Path $repo "Sunrise\src\client\content\statics\statics_footprint_cache.cpp"
$pathSource = Join-Path $repo "Sunrise\src\core\filesystem\path.cpp"
$outputDirectory = Join-Path $env:TEMP "sunrise-statics-footprint-cache-test-$PID"
$objectDirectory = Join-Path $outputDirectory "objects"
$output = Join-Path $outputDirectory "statics_footprint_cache_test.exe"
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
$include = Join-Path $repo "Sunrise\src"
New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
$command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" "{3}" "{4}" "{5}" /Fe:"{6}"' -f $objectDirectory, $vcvars, $include, $testSource, $cacheSource, $pathSource, $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Statics footprint cache test failed to compile."
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "Statics footprint cache test failed."
    }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
