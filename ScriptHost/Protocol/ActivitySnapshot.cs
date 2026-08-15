using System.Globalization;
using System.Text.Json;

namespace Sunrise.ScriptHost.Protocol;

internal sealed record ActivitySnapshot(
    bool Active,
    string WorldPhase,
    ulong TransitionAgeMilliseconds,
    ulong? SessionId,
    bool Joined,
    string? Package,
    int? ActivityIndex,
    string? ArrivalBubbleHash,
    int? ReportedRegion,
    uint HeldEntitySlots)
{
    internal static bool TryParse(JsonElement value, out ActivitySnapshot? snapshot)
    {
        snapshot = null;
        if (value.ValueKind != JsonValueKind.Object
            || !TryBoolean(value, "active", out bool active)
            || !TryString(value, "worldPhase", out string? worldPhase)
            || worldPhase is not ("idle" or "transitioning" or "arrived")
            || !TryUInt64(value, "transitionAgeMs", out ulong transitionAge)
            || !TryBoolean(value, "joined", out bool joined)
            || !TryUInt32(value, "heldEntitySlots", out uint heldEntitySlots)
            || heldEntitySlots > 8_192)
        {
            return false;
        }

        if (!active)
        {
            if (joined || heldEntitySlots != 0
                || !IsNull(value, "sessionId")
                || !IsNull(value, "package")
                || !IsNull(value, "activityIndex")
                || !IsNull(value, "arrivalBubbleHash")
                || !IsNull(value, "reportedRegion"))
            {
                return false;
            }

            snapshot = new ActivitySnapshot(
                false,
                worldPhase,
                transitionAge,
                null,
                false,
                null,
                null,
                null,
                null,
                0);
            return true;
        }

        if (!TryString(value, "sessionId", out string? sessionText)
            || !ulong.TryParse(sessionText, NumberStyles.None, CultureInfo.InvariantCulture, out ulong sessionId)
            || sessionId == 0
            || !TryString(value, "package", out string? package)
            || package is null
            || !ValidPackage(package)
            || !TryNullableInt32(value, "activityIndex", 0, 4_094, out int? activityIndex)
            || !TryNullableHash(value, "arrivalBubbleHash", out string? arrivalBubbleHash)
            || !TryNullableInt32(value, "reportedRegion", -1, 1_022, out int? reportedRegion)
            || reportedRegion is null)
        {
            return false;
        }

        snapshot = new ActivitySnapshot(
            true,
            worldPhase,
            transitionAge,
            sessionId,
            joined,
            package,
            activityIndex,
            arrivalBubbleHash,
            reportedRegion,
            heldEntitySlots);
        return true;
    }

    private static bool TryBoolean(JsonElement value, string name, out bool result)
    {
        result = false;
        if (!value.TryGetProperty(name, out JsonElement property)
            || property.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            return false;
        }
        result = property.GetBoolean();
        return true;
    }

    private static bool TryString(JsonElement value, string name, out string? result)
    {
        result = null;
        return value.TryGetProperty(name, out JsonElement property)
               && property.ValueKind == JsonValueKind.String
               && (result = property.GetString()) is not null;
    }

    private static bool TryUInt64(JsonElement value, string name, out ulong result)
    {
        result = 0;
        return value.TryGetProperty(name, out JsonElement property)
               && property.ValueKind == JsonValueKind.Number
               && property.TryGetUInt64(out result);
    }

    private static bool TryUInt32(JsonElement value, string name, out uint result)
    {
        result = 0;
        return value.TryGetProperty(name, out JsonElement property)
               && property.ValueKind == JsonValueKind.Number
               && property.TryGetUInt32(out result);
    }

    private static bool IsNull(JsonElement value, string name) =>
        value.TryGetProperty(name, out JsonElement property)
        && property.ValueKind == JsonValueKind.Null;

    private static bool TryNullableInt32(
        JsonElement value,
        string name,
        int minimum,
        int maximum,
        out int? result)
    {
        result = null;
        if (!value.TryGetProperty(name, out JsonElement property))
        {
            return false;
        }
        if (property.ValueKind == JsonValueKind.Null)
        {
            return true;
        }
        if (property.ValueKind != JsonValueKind.Number
            || !property.TryGetInt32(out int parsed)
            || parsed < minimum
            || parsed > maximum)
        {
            return false;
        }
        result = parsed;
        return true;
    }

    private static bool TryNullableHash(JsonElement value, string name, out string? result)
    {
        result = null;
        if (!value.TryGetProperty(name, out JsonElement property))
        {
            return false;
        }
        if (property.ValueKind == JsonValueKind.Null)
        {
            return true;
        }
        if (property.ValueKind != JsonValueKind.String
            || property.GetString() is not { Length: 10 } text
            || !text.StartsWith("0x", StringComparison.Ordinal)
            || !uint.TryParse(text.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out _))
        {
            return false;
        }
        result = text;
        return true;
    }

    private static bool ValidPackage(string value) =>
        value.Length is > 0 and <= 40
        && value.All(character =>
            character is >= 'a' and <= 'z'
            || character is >= '0' and <= '9'
            || character == '_');
}
