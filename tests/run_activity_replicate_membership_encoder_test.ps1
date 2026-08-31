$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repo "Sunrise\src"
$temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$outputDirectory = [IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot "sunrise-activity-membership-test-$PID"))
if (-not $outputDirectory.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The test output directory must remain below the system temporary directory."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
try {
    $test = Join-Path $PSScriptRoot "activity_replicate_membership_encoder_test.cpp"
    $encoder = Join-Path $sourceRoot `
        "middleware\bap\activity_message\activity_replicate_membership_encoder.cpp"
    $member = Join-Path $sourceRoot `
        "middleware\bap\activity_message\activity_membership_member_writer.cpp"
    $region = Join-Path $sourceRoot `
        "middleware\bap\activity_message\activity_membership_region_writer.cpp"
    $writer = Join-Path $sourceRoot "middleware\encoding\bit_writer.cpp"
    $command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" "{3}" "{4}" "{5}" "{6}" "{7}" /Fe:activity-membership-test.exe' -f `
        $outputDirectory, $vcvars, $sourceRoot, $test, $encoder, $member, $region, $writer
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "activity membership test failed to compile." }
    & (Join-Path $outputDirectory "activity-membership-test.exe")
    if ($LASTEXITCODE -ne 0) { throw "activity membership test failed." }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
