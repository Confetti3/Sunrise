$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $PSScriptRoot "cpp_test_runner.ps1"
$test = Join-Path $PSScriptRoot "entity_slot_republish_test.cpp"
$source = Join-Path $repo "Sunrise\src\server\bap\encrypted\push\activity\activity_entity_slot_republish.cpp"
$encoder = Join-Path $repo "Sunrise\src\middleware\bap\activity_message\activity_entity_slots_encoder.cpp"
& $runner -Name "entity-slot-republish-test" -Sources @("/DSUNRISE_TESTING", $test, $source, $encoder)
