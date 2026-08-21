$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repository 'build\tests\activity-logic-catalog'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = (& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath).Trim()
if (-not $installation) { throw 'Visual Studio installation was not found.' }
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$source = Join-Path $PSScriptRoot 'activity_logic_catalog_tests.cpp'
$catalog = Join-Path $repository 'Sunrise\src\client\inspection\activity_logic_catalog.cpp'
$include = Join-Path $repository 'Sunrise\src'
$executable = Join-Path $output 'activity_logic_catalog_tests.exe'
$command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}" "{2}" "{3}" /Fo:"{5}\\" /Fe:"{4}"' -f $developerShell, $include, $source, $catalog, $executable, $output
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $executable
exit $LASTEXITCODE
