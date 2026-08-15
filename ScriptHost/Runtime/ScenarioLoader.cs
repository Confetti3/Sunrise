using System.Text.Json;

namespace Sunrise.ScriptHost.Runtime;

public static class ScenarioLoader
{
    private static readonly HashSet<string> TriggerTypes = new(StringComparer.Ordinal)
    {
        "delay",
        "host.start",
        "immediate",
        "activity.incident",
        "signal",
        "world.phase",
    };

    private static readonly HashSet<string> ActionTypes = new(StringComparer.Ordinal)
    {
        "command",
        "log",
        "set",
        "signal",
    };

    public static ScenarioDefinition Load(string path)
    {
        string json = File.ReadAllText(path);
        ScenarioDefinition? scenario = JsonSerializer.Deserialize<ScenarioDefinition>(json, Json.DefaultOptions);
        if (scenario is null)
        {
            throw new InvalidDataException($"Scenario is empty: {path}");
        }

        Validate(scenario, path);
        return scenario;
    }

    public static ScenarioDefinition Parse(string json, string source)
    {
        ScenarioDefinition? scenario = JsonSerializer.Deserialize<ScenarioDefinition>(json, Json.DefaultOptions);
        if (scenario is null)
        {
            throw new InvalidDataException($"Scenario is empty: {source}");
        }

        Validate(scenario, source);
        return scenario;
    }

    private static void Validate(ScenarioDefinition scenario, string source)
    {
        if (scenario.Schema != 1)
        {
            throw new InvalidDataException($"Unsupported scenario schema {scenario.Schema}: {source}");
        }

        if (string.IsNullOrWhiteSpace(scenario.Id)
            || string.IsNullOrWhiteSpace(scenario.Title)
            || string.IsNullOrWhiteSpace(scenario.Start))
        {
            throw new InvalidDataException($"Scenario id, title, and start are required: {source}");
        }

        if (!string.Equals(scenario.Provenance, "authored-prototype", StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Scenario provenance must be 'authored-prototype' until host policy is captured: {source}");
        }

        var nodes = new Dictionary<string, ScenarioNode>(StringComparer.Ordinal);
        foreach (ScenarioNode node in scenario.Nodes)
        {
            if (string.IsNullOrWhiteSpace(node.Id) || !nodes.TryAdd(node.Id, node))
            {
                throw new InvalidDataException($"Scenario node ids must be nonempty and unique: {source}");
            }

            if (!TriggerTypes.Contains(node.Trigger.Type))
            {
                throw new InvalidDataException($"Unknown trigger '{node.Trigger.Type}' in node '{node.Id}'.");
            }

            if (node.Trigger.Type == "delay" && node.Trigger.Milliseconds is null or < 0)
            {
                throw new InvalidDataException($"Delay node '{node.Id}' needs a nonnegative milliseconds value.");
            }

            if ((node.Trigger.Type == "activity.incident"
                    || node.Trigger.Type == "signal"
                    || node.Trigger.Type == "world.phase")
                && string.IsNullOrWhiteSpace(node.Trigger.Value))
            {
                throw new InvalidDataException($"Trigger '{node.Trigger.Type}' in node '{node.Id}' needs a value.");
            }

            foreach (ActionDefinition action in node.Actions)
            {
                ValidateAction(node.Id, action);
            }
        }

        if (!nodes.ContainsKey(scenario.Start))
        {
            throw new InvalidDataException($"Scenario start node '{scenario.Start}' does not exist: {source}");
        }

        foreach (ScenarioNode node in nodes.Values)
        {
            if (node.Next is not null && !nodes.ContainsKey(node.Next))
            {
                throw new InvalidDataException($"Node '{node.Id}' points to missing node '{node.Next}'.");
            }
        }
    }

    private static void ValidateAction(string nodeId, ActionDefinition action)
    {
        if (!ActionTypes.Contains(action.Type))
        {
            throw new InvalidDataException($"Unknown action '{action.Type}' in node '{nodeId}'.");
        }

        switch (action.Type)
        {
            case "command" when string.IsNullOrWhiteSpace(action.Capability):
                throw new InvalidDataException($"Command action in node '{nodeId}' needs a capability.");
            case "log" when action.Message is null:
                throw new InvalidDataException($"Log action in node '{nodeId}' needs a message.");
            case "set" when string.IsNullOrWhiteSpace(action.Name) || action.Value is null:
                throw new InvalidDataException($"Set action in node '{nodeId}' needs name and value.");
            case "signal" when string.IsNullOrWhiteSpace(action.Name):
                throw new InvalidDataException($"Signal action in node '{nodeId}' needs a name.");
        }
    }
}
