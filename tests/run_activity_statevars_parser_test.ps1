$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "activity-statevars-parser-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "activity_statevars_parser_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\content\activity\activity_statevars.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\content\packages\tables\definition_index_table.cpp")
    )
