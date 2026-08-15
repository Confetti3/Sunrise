using System.Text.Json;

namespace Sunrise.ScriptHost.Protocol;

internal sealed record PlacedContentAuthorityObservation(
    ulong DecodeCount,
    ulong ForcedReadCount,
    ulong LastDecoderForcedReads,
    ulong DroppedCount,
    bool LastDecoderSucceeded)
{
    internal static bool TryParse(JsonElement message, out PlacedContentAuthorityObservation? observation)
    {
        observation = null;

        if (!message.TryGetProperty("protocol", out JsonElement protocolElement)
            || protocolElement.ValueKind != JsonValueKind.Number
            || !protocolElement.TryGetUInt32(out uint protocol)
            || protocol != 1)
        {
            return false;
        }

        if (!message.TryGetProperty("type", out JsonElement typeElement)
            || typeElement.ValueKind != JsonValueKind.String
            || !string.Equals(typeElement.GetString(), "placed-content.authority", StringComparison.Ordinal))
        {
            return false;
        }

        if (!TryGetUInt64(message, "decodeCount", out ulong decodeCount)
            || !TryGetUInt64(message, "forcedReadCount", out ulong forcedReadCount)
            || !TryGetUInt64(message, "lastDecoderForcedReads", out ulong lastDecoderForcedReads)
            || !TryGetUInt64(message, "droppedCount", out ulong droppedCount))
        {
            return false;
        }

        if (!message.TryGetProperty("lastDecoderSucceeded", out JsonElement succeededElement)
            || (succeededElement.ValueKind != JsonValueKind.True
                && succeededElement.ValueKind != JsonValueKind.False))
        {
            return false;
        }

        observation = new PlacedContentAuthorityObservation(
            decodeCount,
            forcedReadCount,
            lastDecoderForcedReads,
            droppedCount,
            succeededElement.GetBoolean());
        return true;
    }

    private static bool TryGetUInt64(JsonElement message, string name, out ulong value)
    {
        value = 0;
        return message.TryGetProperty(name, out JsonElement element)
            && element.ValueKind == JsonValueKind.Number
            && element.TryGetUInt64(out value);
    }
}
