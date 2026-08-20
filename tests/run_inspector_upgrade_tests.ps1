$ErrorActionPreference = 'Stop'

$repository = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repository 'build\tests\inspector-upgrade'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$developerShell = 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat'
$source = Join-Path $PSScriptRoot 'inspector_upgrade_tests.cpp'
$model = Join-Path $repository 'Sunrise\src\client\inspection\world_inspection_model.cpp'
$primitives = Join-Path $repository 'Sunrise\src\client\ui\world_inspector\world_debug_primitives.cpp'
$catalog = Join-Path $repository 'Sunrise\src\client\inspection\activity_graph_catalog.cpp'
$layout = Join-Path $repository 'Sunrise\src\client\ui\world_inspector\world_inspector_graph_layout.cpp'
$include = Join-Path $repository 'Sunrise\src'
$executable = Join-Path $output 'inspector_upgrade_tests.exe'

$command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}" "{2}" "{3}" "{4}" "{5}" "{6}" /Fo:"{8}\\" /Fe:"{7}"' -f $developerShell, $include, $source, $model, $primitives, $catalog, $layout, $executable, $output
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $executable
exit $LASTEXITCODE
