$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$settingsPath = Join-Path $repo 'Sunrise\resources\default_settings.json'
$settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
$consoleKey = [string]$settings.client.ui.console_toggle_key
if ([string]::IsNullOrWhiteSpace($consoleKey)) {
    throw 'The shipped console toggle key is missing.'
}
if ($consoleKey -eq [string]$settings.client.ui.toggle_key) {
    throw "The shipped console key '$consoleKey' collides with the Sunrise menu key."
}
foreach ($binding in $settings.state.account.settings.key_bindings.PSObject.Properties) {
    foreach ($slot in @('primary', 'secondary')) {
        $value = [string]$binding.Value.$slot
        if (-not [string]::IsNullOrWhiteSpace($value) `
            -and $value.Equals($consoleKey, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "The shipped console key '$consoleKey' collides with gameplay binding '$($binding.Name).$slot'."
        }
    }
}
Write-Output 'console-default-binding-ok'
