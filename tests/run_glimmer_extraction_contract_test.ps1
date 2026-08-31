$repo = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "cpp_test_runner.ps1") `
    -Name "glimmer-extraction-contract-test" `
    -Sources @((Join-Path $PSScriptRoot "glimmer_extraction_contract_test.cpp"))
