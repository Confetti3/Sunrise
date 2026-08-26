$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$testSource = Join-Path $PSScriptRoot "activity_preview_provider_test.cpp"
$sources = @(
    (Join-Path $repo "Sunrise\src\client\inspection\providers\activity_graph_inspection.cpp"),
    (Join-Path $repo "Sunrise\src\client\inspection\providers\activity_logic_inspection.cpp"),
    (Join-Path $repo "Sunrise\src\client\inspection\activity_graph_catalog.cpp"),
    (Join-Path $repo "Sunrise\src\client\inspection\activity_logic_catalog.cpp"),
    (Join-Path $repo "Sunrise\src\client\inspection\inspection_descriptors.cpp"),
    (Join-Path $repo "Sunrise\src\client\inspection\world_inspection_model.cpp")
)
$include = Join-Path $repo "Sunrise\src"
$outputDirectory = Join-Path $env:TEMP "sunrise-activity-preview-provider-test-$PID"
$objectDirectory = Join-Path $outputDirectory "objects"
$output = Join-Path $outputDirectory "activity_preview_provider_test.exe"
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
$quotedSources = ($sources | ForEach-Object { '"{0}"' -f $_ }) -join ' '
$command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" "{3}" {4} /Fe:"{5}"' -f $objectDirectory, $vcvars, $include, $testSource, $quotedSources, $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Activity preview provider test failed to compile."
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "Activity preview provider test failed."
    }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
