[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'activity', 'switch', 'placed-content', 'logs', 'signal', 'reset', 'scenario')]
    [string]$Command = 'status',

    [Parameter(Position = 1)]
    [string]$Action,

    [Parameter(Position = 2)]
    [string]$Target,

    [Parameter(Position = 3)]
    [string]$Value,

    [ValidateRange(1, 500)]
    [int]$Last = 50,

    [string]$PipeName = 'sunrise-script-host-v1-operator',

    [ValidateRange(1, 60)]
    [int]$TimeoutSeconds = 5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-DefinitionIndex {
    param([Parameter(Mandatory)][string]$Text)

    try {
        if ($Text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            return [Convert]::ToUInt16($Text.Substring(2), 16)
        }
        return [Convert]::ToUInt16($Text, 10)
    }
    catch {
        throw "Definition index '$Text' is not a UInt16 decimal or 0x-prefixed value."
    }
}

$request = [ordered]@{
    protocol = 1
}

switch ($Command) {
    'status' {
        $request.command = 'runtime.status'
    }
    'activity' {
        $request.command = 'activity.snapshot'
    }
    'switch' {
        if ($Action -notin @('get', 'set') -or [string]::IsNullOrWhiteSpace($Target)) {
            throw 'Usage: sunrise-lab.ps1 switch get <definition> | switch set <definition> <value>'
        }
        $request.definitionIndex = Convert-DefinitionIndex $Target
        if ($Action -eq 'get') {
            $request.command = 'gameplay-switch.read'
        }
        else {
            if ([string]::IsNullOrWhiteSpace($Value)) {
                throw 'switch set requires a value: 0, 1, or 2.'
            }
            $parsedValue = 0
            if (-not [int]::TryParse($Value, [ref]$parsedValue) -or $parsedValue -lt 0 -or $parsedValue -gt 2) {
                throw "Switch value '$Value' must be 0, 1, or 2."
            }
            $request.command = 'gameplay-switch.set'
            $request.value = $parsedValue
        }
    }
    'placed-content' {
        $request.command = 'placed-content.authority.observe'
    }
    'logs' {
        $request.command = 'logs.tail'
        $request.last = $Last
    }
    'signal' {
        if ([string]::IsNullOrWhiteSpace($Action)) {
            throw 'signal requires a name, for example: sunrise-lab.ps1 signal wave.cleared'
        }
        $request.command = 'scenario.signal'
        $request.name = $Action
    }
    'reset' {
        $request.command = 'scenario.reset'
    }
    'scenario' {
        if ($Action -notin @('status', 'reset')) {
            throw 'Usage: sunrise-lab.ps1 scenario status | scenario reset'
        }
        $request.command = "scenario.$Action"
    }
}

$client = [System.IO.Pipes.NamedPipeClientStream]::new(
    '.',
    $PipeName,
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)
try {
    $client.Connect($TimeoutSeconds * 1000)
    $encoding = [System.Text.UTF8Encoding]::new($false, $true)
    $writer = [System.IO.StreamWriter]::new($client, $encoding, 1024, $true)
    $reader = [System.IO.StreamReader]::new($client, $encoding, $false, 1024, $true)
    try {
        $writer.WriteLine(($request | ConvertTo-Json -Compress -Depth 8))
        $writer.Flush()
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw 'The script host closed the operator pipe without a response.'
        }
        $reply = $line | ConvertFrom-Json
        $reply | ConvertTo-Json -Depth 16
        if (-not $reply.ok) {
            exit 1
        }
    }
    finally {
        $reader.Dispose()
        $writer.Dispose()
    }
}
finally {
    $client.Dispose()
}
