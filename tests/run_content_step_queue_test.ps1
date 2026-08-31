$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repo "Sunrise\src"
$temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $temporaryRoot "sunrise-content-step-queue-test-$PID"))
if (-not $outputDirectory.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The test output directory must remain below the system temporary directory."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
try {
    $test = Join-Path $PSScriptRoot "content_step_queue_test.cpp"
    $queue = Join-Path $sourceRoot "server\gameplay\mission\content_step_queue.cpp"
    $command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /I"{2}" "{3}" "{4}" /Fe:content-step-queue-test.exe' -f $outputDirectory, $vcvars, $sourceRoot, $test, $queue
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "content-step queue test failed to compile." }
    & (Join-Path $outputDirectory "content-step-queue-test.exe")
    if ($LASTEXITCODE -ne 0) { throw "content-step queue test failed." }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
