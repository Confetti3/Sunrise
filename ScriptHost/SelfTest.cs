using System.Text.Json;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost;

public static class SelfTest
{
    public static async Task RunAsync()
    {
        string protocolFrame = JsonSerializer.Serialize(
            new { protocol = 1, type = "command.request" },
            Json.ProtocolOptions);
        if (protocolFrame.Contains('\n') || protocolFrame.Contains('\r'))
        {
            throw new InvalidOperationException(
                "Named-pipe protocol JSON must occupy exactly one physical line.");
        }

        const string scenarioJson = """
        {
          "schema": 1,
          "id": "self-test",
          "title": "Self test",
          "provenance": "authored-prototype",
          "start": "start",
          "parameters": { "value": "ready" },
          "nodes": [
            {
              "id": "start",
              "trigger": { "type": "host.start" },
              "actions": [
                { "type": "set", "name": "copy", "value": "${value}" }
              ],
              "next": "incident"
            },
            {
              "id": "incident",
              "trigger": { "type": "activity.incident", "value": "42" },
              "actions": [
                { "type": "command", "capability": "test.echo", "payload": { "value": "${copy}" } }
              ]
            }
          ]
        }
        """;

        const string manifestJson = """
        {
          "schema": 1,
          "build": "self-test",
          "capabilities": [
            {
              "id": "test.echo",
              "status": "available",
              "owner": "self-test",
              "evidence": "In-memory dispatcher"
            }
          ]
        }
        """;

        string temporaryRoot = Path.Combine(Path.GetTempPath(), "sunrise-script-host-self-test", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        try
        {
            string manifestPath = Path.Combine(temporaryRoot, "manifest.json");
            await File.WriteAllTextAsync(manifestPath, manifestJson).ConfigureAwait(false);
            ScenarioDefinition scenario = ScenarioLoader.Parse(scenarioJson, "self-test");
            CapabilityCatalog catalog = CapabilityCatalog.Load(manifestPath);
            catalog.ReplacePluginCapabilities(new[] { "test.echo" });
            var dispatcher = new EchoDispatcher();
            var store = new CheckpointStore(Path.Combine(temporaryRoot, "checkpoint.json"));
            var runtime = new MissionRuntime(scenario, catalog, dispatcher, store);

            await runtime.InitializeAsync(CancellationToken.None).ConfigureAwait(false);
            await runtime.PublishEventAsync(HostEvent.Start, CancellationToken.None).ConfigureAwait(false);
            if (runtime.IsCompleted || dispatcher.LastValue is not null)
            {
                throw new InvalidOperationException("Incident trigger ran before an incident was published.");
            }

            await runtime.PublishEventAsync(
                new HostEvent(
                    "activity.incident",
                    new Dictionary<string, string>(StringComparer.Ordinal)
                    {
                        ["primaryTarget"] = "41",
                    }),
                CancellationToken.None).ConfigureAwait(false);
            if (runtime.IsCompleted || dispatcher.LastValue is not null)
            {
                throw new InvalidOperationException("Incident trigger accepted a nonmatching primary target.");
            }

            await runtime.PublishEventAsync(
                new HostEvent(
                    "activity.incident",
                    new Dictionary<string, string>(StringComparer.Ordinal)
                    {
                        ["primaryTarget"] = "042",
                    }),
                CancellationToken.None).ConfigureAwait(false);
            if (!runtime.IsCompleted || dispatcher.LastValue != "ready")
            {
                throw new InvalidOperationException("Mission runtime did not complete the deterministic self-test.");
            }
        }
        finally
        {
            Directory.Delete(temporaryRoot, recursive: true);
        }
    }

    private sealed class EchoDispatcher : ICommandDispatcher
    {
        public string? LastValue { get; private set; }

        public Task<CommandResult> DispatchAsync(
            string capability,
            JsonElement payload,
            IReadOnlyDictionary<string, string> variables,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!string.Equals(capability, "test.echo", StringComparison.Ordinal))
            {
                return Task.FromResult(new CommandResult(false, "unsupported", capability));
            }

            LastValue = payload.GetProperty("value").GetString();
            return Task.FromResult(new CommandResult(true, "ok", null));
        }
    }
}
