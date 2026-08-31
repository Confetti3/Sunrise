$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "console-safety-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "console_safety_test.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\registry\console_registry.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\parser\console_line_parse.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\invoke\console_invoke.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\queue\console_queue.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\output\console_format.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\output\console_output.cpp")
    )
