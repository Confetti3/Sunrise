$repo = Split-Path -Parent $PSScriptRoot
$world = Join-Path $repo "Sunrise\src\server\gameplay\physics\world"
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "compiled-mission-policy-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "compiled_mission_policy_test.cpp"),
        (Join-Path $repo "Sunrise\src\server\gameplay\mission\compiled_mission_policy.cpp"),
        (Join-Path $repo "Sunrise\src\server\gameplay\mission\content_step_queue.cpp"),
        (Join-Path $world "activity_policy.cpp"),
        (Join-Path $world "actor_store.cpp"),
        (Join-Path $world "actor_store_pose.cpp"),
        (Join-Path $world "actor_store_restore.cpp"),
        (Join-Path $world "actor_store_generations.cpp"),
        (Join-Path $world "authority_manager.cpp"),
        (Join-Path $world "command_queue.cpp"),
        (Join-Path $world "host_command.cpp"),
        (Join-Path $world "world_runner.cpp"),
        (Join-Path $world "world_runner_acl.cpp"),
        (Join-Path $world "world_runner_commands.cpp"),
        (Join-Path $world "world_runner_events.cpp"),
        (Join-Path $world "world_runner_executor.cpp"),
        (Join-Path $world "world_runner_hash.cpp"),
        (Join-Path $world "world_runner_restore.cpp"),
        (Join-Path $world "world_runner_transaction.cpp"),
        (Join-Path $world "world_types.cpp")
    )
