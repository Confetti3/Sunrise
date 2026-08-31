param([string]$AuthoritativeJson)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $PSScriptRoot 'build\sqlite-bootstrap'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw 'A Visual Studio C++ installation was not found.'
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & cmd.exe /d /s /c "`"$vcvars`" >nul && set" 2>$null
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator),
                                               $line.Substring($separator + 1))
    }
}

$settingsSources = Get-ChildItem -LiteralPath (Join-Path $repo 'Sunrise\src\core\settings') `
    -Recurse -Filter '*.cpp' | Where-Object {
        $_.Name -notin @('settings_runtime.cpp', 'settings_upgrade.cpp')
    } | ForEach-Object FullName
$sources = @(
    $settingsSources
    (Join-Path $repo 'Sunrise\src\state\account\account_state.cpp')
    (Join-Path $repo 'Sunrise\src\state\account\inventory\inventory_state.cpp')
    (Join-Path $repo 'Sunrise\src\state\account\settings\settings_state.cpp')
    (Join-Path $repo 'Sunrise\src\state\activity\defaults\activity_defaults_validation.cpp')
    (Join-Path $repo 'Sunrise\src\state\entitlements\entitlement_defaults.cpp')
    (Join-Path $repo 'Sunrise\src\state\entitlements\entitlement_validation.cpp')
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\database.cpp')
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\schema.cpp')
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\bootstrap_snapshot.cpp')
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\account_settings_codec.cpp')
    (Join-Path $repo 'Sunrise\src\state\persistence\sqlite\account_snapshot.cpp')
    (Join-Path $PSScriptRoot 'sqlite_bootstrap_snapshot_test.cpp')
)
$exe = Join-Path $build 'sqlite_bootstrap_snapshot_test.exe'
$arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/utf-8',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', "/I$(Join-Path $repo 'Sunrise\src')",
    "/Fo:$build\", "/Fe:$exe") + $sources + @('/link', '/STACK:8388608',
        'winsqlite3.lib', 'kernel32.lib')
& cl.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw "cl.exe failed with exit code $LASTEXITCODE."
}

if ([string]::IsNullOrWhiteSpace($AuthoritativeJson)) {
    $AuthoritativeJson = Join-Path $repo 'Sunrise\resources\default_settings.json'
}
& $exe $repo $AuthoritativeJson
if ($LASTEXITCODE -ne 0) {
    throw "sqlite bootstrap snapshot test failed with exit code $LASTEXITCODE."
}
