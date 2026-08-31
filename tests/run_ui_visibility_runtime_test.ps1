$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "ui-visibility-runtime-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "ui_visibility_runtime_test.cpp"),
        (Join-Path $repo "Sunrise\src\core\ui\runtime\ui_visibility_runtime.cpp")
    )
