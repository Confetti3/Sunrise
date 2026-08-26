$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$testSource = Join-Path $PSScriptRoot "current_location_domain_cache_test.cpp"
$cacheSource = Join-Path $repo "Sunrise\src\client\inspection\current_location_domain_cache.cpp"
$logicSource = Join-Path $repo "Sunrise\src\client\inspection\activity_logic_catalog.cpp"
$graphSource = Join-Path $repo "Sunrise\src\client\inspection\activity_graph_catalog.cpp"
$bubbleSource = Join-Path $repo "Sunrise\src\client\inspection\bubble_bounds_catalog.cpp"
$pathSource = Join-Path $repo "Sunrise\src\core\filesystem\path.cpp"
$outputDirectory = Join-Path $env:TEMP "sunrise-current-location-domain-cache-test-$PID"
$objectDirectory = Join-Path $outputDirectory "objects"
$output = Join-Path $outputDirectory "current_location_domain_cache_test.exe"
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
$command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" "{3}" "{4}" "{5}" "{6}" "{7}" "{8}" /Fe:"{9}"' -f $objectDirectory, $vcvars, $include, $testSource, $cacheSource, $logicSource, $graphSource, $bubbleSource, $pathSource, $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Current-location domain cache test failed to compile."
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "Current-location domain cache test failed."
    }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
