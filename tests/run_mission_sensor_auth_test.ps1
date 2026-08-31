$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "mission-sensor-auth-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "mission_sensor_auth_test.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\encoding\bit_writer.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\encoding\bit_reader.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\bap\activity_message\activity_sense_update_parser.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\bap\activity_message\activity_sensor_auth_encoder.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\bap\activity_message\activity_sensor_auth_blocks.cpp"),
        (Join-Path $repo "Sunrise\src\middleware\bap\activity_message\activity_sensor_auth_bodies.cpp")
    )
