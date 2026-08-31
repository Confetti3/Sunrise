$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repo 'Sunrise\src'
$build = Join-Path ([IO.Path]::GetTempPath()) "sunrise-console-endpoint-loopback-$PID"
New-Item -ItemType Directory -Force -Path $build | Out-Null
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    $sources = @(
        (Join-Path $PSScriptRoot 'console_endpoint_loopback_test.cpp'),
        (Join-Path $sourceRoot 'server\console_endpoint\console_endpoint.cpp'),
        (Join-Path $sourceRoot 'server\console_endpoint\protocol\console_protocol.cpp'),
        (Join-Path $sourceRoot 'server\console_endpoint\replies\console_reply_table.cpp'),
        (Join-Path $sourceRoot 'core\console\registry\console_registry.cpp'),
        (Join-Path $sourceRoot 'core\console\parser\console_line_parse.cpp'),
        (Join-Path $sourceRoot 'core\console\invoke\console_invoke.cpp'),
        (Join-Path $sourceRoot 'core\console\queue\console_queue.cpp')
    ) | ForEach-Object { '"{0}"' -f $_ }
    $exe = Join-Path $build 'console-endpoint-loopback-test.exe'
    $command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /std:c++20 /W4 /WX /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{2}" {3} /Fe:"{4}" /link ws2_32.lib' -f $build, $vcvars, $sourceRoot, ($sources -join ' '), $exe
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'console endpoint loopback test failed to compile.' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "console endpoint loopback test failed with exit code $LASTEXITCODE." }
} finally {
    Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction SilentlyContinue
}
