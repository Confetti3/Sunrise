$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found; install Visual Studio Build Tools with the C++ workload."
}
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vsPath = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw 'A Visual Studio C++ installation was not found.'
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "The x64 Visual C++ environment was not found at $vcvars."
}

# Import the environment produced by the Visual Studio batch setup into this PowerShell process.
$environment = & cmd.exe /d /s /c "`"$vcvars`" >nul && set" 2>$null
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator),
                                               $line.Substring($separator + 1))
    }
}

$exe = Join-Path $build 'sqlite_account_snapshot_test.exe'
$sources = @(
    (Join-Path $repo 'Sunrise\src\state\account\account_state.cpp'),
    (Join-Path $repo 'Sunrise\src\state\account\inventory\inventory_state.cpp'),
    (Join-Path $repo 'Sunrise\src\state\account\settings\settings_state.cpp'),
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\database.cpp'),
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\schema.cpp'),
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\account_settings_codec.cpp'),
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\account_snapshot.cpp'),
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\state_store.cpp'),
    (Join-Path $PSScriptRoot 'sqlite_account_snapshot_test.cpp')
)

$arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/utf-8',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', "/I$(Join-Path $repo 'Sunrise\src')",
    "/Fo:$build\", "/Fe:$exe") + $sources + @('/link', 'winsqlite3.lib', 'kernel32.lib')
& cl.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw "cl.exe failed with exit code $LASTEXITCODE."
}
& $exe
if ($LASTEXITCODE -ne 0) {
    throw "sqlite account snapshot test failed with exit code $LASTEXITCODE."
}
