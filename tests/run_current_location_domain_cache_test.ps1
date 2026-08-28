$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "current-location-domain-cache-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "current_location_domain_cache_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\current_location_domain_cache.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\activity_logic_catalog.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\activity_graph_catalog.cpp"),
        (Join-Path $repo "Sunrise\src\client\inspection\bubble_bounds_catalog.cpp"),
        (Join-Path $repo "Sunrise\src\core\filesystem\path.cpp")
    )
