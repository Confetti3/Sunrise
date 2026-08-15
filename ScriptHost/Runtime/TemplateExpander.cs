using System.Text.Json;
using System.Text.Json.Nodes;

namespace Sunrise.ScriptHost.Runtime;

public static class TemplateExpander
{
    public static JsonElement Expand(JsonElement? source, IReadOnlyDictionary<string, string> variables)
    {
        if (source is null)
        {
            return JsonSerializer.SerializeToElement(new { }, Json.DefaultOptions);
        }

        JsonNode? node = JsonNode.Parse(source.Value.GetRawText());
        JsonNode? expanded = ExpandNode(node, variables);
        return JsonSerializer.SerializeToElement(expanded, Json.DefaultOptions);
    }

    public static string Expand(string source, IReadOnlyDictionary<string, string> variables)
    {
        string result = source;
        foreach ((string key, string value) in variables)
        {
            result = result.Replace($"${{{key}}}", value, StringComparison.Ordinal);
        }

        return result;
    }

    private static JsonNode? ExpandNode(JsonNode? node, IReadOnlyDictionary<string, string> variables)
    {
        switch (node)
        {
            case JsonValue value when value.TryGetValue(out string? text) && text is not null:
                return JsonValue.Create(Expand(text, variables));
            case JsonObject objectNode:
                foreach (string key in objectNode.Select(item => item.Key).ToArray())
                {
                    JsonNode? current = objectNode[key];
                    JsonNode? replacement = ExpandNode(current, variables);
                    if (!ReferenceEquals(current, replacement))
                    {
                        objectNode[key] = replacement;
                    }
                }

                return objectNode;
            case JsonArray arrayNode:
                for (int index = 0; index < arrayNode.Count; ++index)
                {
                    JsonNode? current = arrayNode[index];
                    JsonNode? replacement = ExpandNode(current, variables);
                    if (!ReferenceEquals(current, replacement))
                    {
                        arrayNode[index] = replacement;
                    }
                }

                return arrayNode;
            default:
                return node;
        }
    }
}
