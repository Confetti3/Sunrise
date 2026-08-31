$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "console-endpoint-protocol-test" `
    -Sources @(
        (Join-Path $PSScriptRoot "console_endpoint_protocol_test.cpp"),
        (Join-Path $repo "Sunrise\src\server\console_endpoint\protocol\console_protocol.cpp"),
        (Join-Path $repo "Sunrise\src\server\console_endpoint\replies\console_reply_table.cpp"),
        (Join-Path $repo "Sunrise\src\core\console\registry\console_registry.cpp")
    )
