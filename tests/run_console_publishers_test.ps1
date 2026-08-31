$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "console-publishers-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "console_publishers_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\movement\movement_console.cpp"),
        (Join-Path $repo "Sunrise\src\server\character\character_console.cpp")
    )
