$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "world-catalog-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "world_catalog_test.cpp"),
        (Join-Path $repo "Sunrise\src\state\build_data\worlds\world_catalog.cpp")
    )
