$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $PSScriptRoot "cpp_test_runner.ps1"
$test = Join-Path $PSScriptRoot "mission_console_test.cpp"
$source = Join-Path $repo "Sunrise\src\server\gameplay\mission\mission_console.cpp"
& $runner -Name "mission-console-test" -Sources @($test, $source)
