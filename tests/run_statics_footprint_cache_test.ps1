$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "statics-footprint-cache-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "statics_footprint_cache_test.cpp"),
        (Join-Path $repo "Sunrise\src\client\content\statics\statics_footprint_cache.cpp"),
        (Join-Path $repo "Sunrise\src\core\filesystem\path.cpp")
    )
