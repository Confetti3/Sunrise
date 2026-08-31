$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $PSScriptRoot "cpp_test_runner.ps1"
$sourceRoot = Join-Path $repo "Sunrise\src"
$test = Join-Path $PSScriptRoot "entity_slot_notification_frame_test.cpp"
$sources = @(
    $test,
    (Join-Path $sourceRoot "server\bap\encrypted\push\activity\activity_entity_slot_push.cpp"),
    (Join-Path $sourceRoot "server\bap\encrypted\push\activity\activity_notification_frame.cpp"),
    (Join-Path $sourceRoot "middleware\bap\activity_message\activity_entity_slots_encoder.cpp"),
    (Join-Path $sourceRoot "middleware\bap\activity_message\activity_message_notification_encoder.cpp"),
    (Join-Path $sourceRoot "middleware\bap\bap_frame.cpp"),
    (Join-Path $sourceRoot "middleware\secure_channel\encrypted_frame.cpp"),
    "bcrypt.lib"
)
& $runner -Name "entity-slot-notification-frame-test" -Sources $sources
