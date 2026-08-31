$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "scenario-slot-classification-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "scenario_slot_classification_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\content\scenarios\scenario_slot_classification.cpp")
    )
