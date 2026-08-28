$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "world-inspector-overview-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "world_inspector_overview_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\ui\world_inspector\world_inspector_overview.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\inspection_descriptors.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\world_inspection_model.cpp")
    )
