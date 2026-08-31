$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "enemy-wave-queue-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "enemy_wave_queue_test.cpp"),
        (Join-Path $repo "Sunrise\src\server\gameplay\mission\enemy_wave_queue.cpp")
    )
