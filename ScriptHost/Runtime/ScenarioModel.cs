using System.Text.Json;

namespace Sunrise.ScriptHost.Runtime;

public sealed record ScenarioDefinition
{
    public int Schema { get; init; }
    public required string Id { get; init; }
    public required string Title { get; init; }
    public required string Provenance { get; init; }
    public required string Start { get; init; }
    public IReadOnlyDictionary<string, string> Parameters { get; init; } = new Dictionary<string, string>();
    public required IReadOnlyList<ScenarioNode> Nodes { get; init; }
}

public sealed record ScenarioNode
{
    public required string Id { get; init; }
    public required TriggerDefinition Trigger { get; init; }
    public required IReadOnlyList<ActionDefinition> Actions { get; init; }
    public string? Next { get; init; }
}

public sealed record TriggerDefinition
{
    public required string Type { get; init; }
    public string? Value { get; init; }
    public int? Milliseconds { get; init; }
}

public sealed record ActionDefinition
{
    public required string Type { get; init; }
    public string? Message { get; init; }
    public string? Capability { get; init; }
    public string? Name { get; init; }
    public JsonElement? Value { get; init; }
    public JsonElement? Payload { get; init; }
}

public sealed record HostEvent(
    string Type,
    IReadOnlyDictionary<string, string> Fields,
    JsonElement? Payload = null)
{
    public static HostEvent Start { get; } = new(
        "host.start",
        new Dictionary<string, string>(StringComparer.Ordinal));
}
