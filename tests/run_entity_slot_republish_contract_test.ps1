$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Read-Source([string]$Relative) {
    return [IO.File]::ReadAllText((Join-Path $repo $Relative))
}
function Assert-Contains([string]$Text, [string]$Pattern, [string]$Label) {
    if (-not [regex]::IsMatch($Text, $Pattern, [Text.RegularExpressions.RegexOptions]::Singleline)) {
        throw "Missing contract: $Label"
    }
}
function Assert-Count([string]$Text, [string]$Pattern, [int]$Expected, [string]$Label) {
    $actual = [regex]::Matches($Text, $Pattern).Count
    if ($actual -ne $Expected) { throw "$Label count was $actual, expected $Expected" }
}

$ui = Read-Source "Sunrise\src\client\ui\world_inspector\world_inspector.cpp"
Assert-Contains $ui 'ImGui::MenuItem\("Republish held entity slots once"\).*?request_entity_slot_republish\(\)' "UI manual request action"
Assert-Contains $ui 'entity_slot_republish_status\(\).*?pendingToken.*?stagedToken.*?delivered.*?deliveredToken' "UI token and delivery status"
Assert-Contains $ui 'entitySlotRepublishToken == 0.*?noPrivateRejected != 0.*?exact private-current binding' "UI refusal state"
Assert-Contains $ui 'Manual diagnostic only; duplicate type-0 semantics are unproven' "UI diagnostic warning"

$keepalive = Read-Source "Sunrise\src\server\bap\encrypted\push\activity\activity_keepalive_push.cpp"
$transaction = Read-Source "Sunrise\src\server\bap\encrypted\activity_transaction\activity_transaction_notifications.cpp"
$encrypted = Read-Source "Sunrise\src\server\bap\encrypted\encrypted_runtime.cpp"
$roster = Read-Source "Sunrise\src\server\bap\encrypted\push\activity\activity_roster_push.cpp"
Assert-Count $keepalive 'append_roster_notification\s*\(' 3 "keepalive roster caller"
Assert-Count $transaction 'append_roster_notification\s*\(' 2 "transaction roster caller"
Assert-Contains $keepalive '!published \|\| framedSize == 0 \|\| framedSize > response.size\(\).*?discard_staged_roster\(session\).*?return false' "keepalive copy/capacity discard"
Assert-Contains $keepalive 'response\[index\] = scratch\.framed\[index\].*?written = framedSize.*?session\.sendNonce = nextSendNonce.*?commit_staged_entity_slot_republish\(session\).*?commit_staged_roster\(session\)' "keepalive copy then commit"
Assert-Contains $encrypted 'std::copy_n\(scratch\.framed\.begin\(\), framedSize, response\.begin\(\)\).*?written = framedSize.*?session\.sendNonce = nextSendNonce.*?commit_staged_entity_slot_republish\(session\).*?publish_connection_fields\(session.*?commit_staged_roster\(session\)' "transaction copy/rebind/commit ordering"
Assert-Contains $encrypted 'if \(!handled\).*?discard_staged_roster\(session\).*?discard_staged_advertisement\(session\)' "transaction failure discard"
Assert-Contains $roster 'append_entity_slot_roster_pair\(.*?append_entity_slot_notification\(.*?append_roster_body\(.*?discard_staged_roster\(session\).*?stage_publication\(session, republish\)' "central atomic type0 roster wrapper"

Write-Host "Entity-slot UI and roster wiring contracts passed."
