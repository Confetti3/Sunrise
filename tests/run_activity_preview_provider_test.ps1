$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "activity-preview-provider-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "activity_preview_provider_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\providers\activity_graph_inspection.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\providers\activity_logic_inspection.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\activity_graph_catalog.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\activity_logic_catalog.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\inspection_descriptors.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\world_inspection_model.cpp")
    )
