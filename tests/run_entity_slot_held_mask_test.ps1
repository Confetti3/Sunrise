$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $PSScriptRoot "cpp_test_runner.ps1"
$test = Join-Path $PSScriptRoot "entity_slot_held_mask_test.cpp"
$source = Join-Path $repo "Sunrise\src\state\activity\entity_slots\transactions\activity_entity_slot_commit.cpp"
& $runner -Name "entity-slot-held-mask-test" -Sources @($test, $source)
