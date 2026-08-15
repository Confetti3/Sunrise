using System.Text.Json;
using Sunrise.ScriptHost.Protocol;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost.Hosting;

public sealed class ScriptHostService
{
    private readonly NamedPipeBridge _bridge;
    private readonly CapabilityCatalog _capabilities;
    private readonly MissionRuntime _runtime;
    private readonly CompositeCommandDispatcher _dispatcher;

    public ScriptHostService(
        NamedPipeBridge bridge,
        CapabilityCatalog capabilities,
        MissionRuntime runtime,
        CompositeCommandDispatcher dispatcher)
    {
        _bridge = bridge;
        _capabilities = capabilities;
        _runtime = runtime;
        _dispatcher = dispatcher;
    }

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        _bridge.MessageReceived += OnMessageAsync;
        _bridge.ConnectionChanged += OnConnectionChangedAsync;

        await _runtime.InitializeAsync(cancellationToken).ConfigureAwait(false);
        await _runtime.PublishEventAsync(HostEvent.Start, cancellationToken).ConfigureAwait(false);

        Task bridgeTask = _bridge.RunAsync(cancellationToken);
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(100));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                await _runtime.TickAsync(cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            await bridgeTask.ConfigureAwait(false);
            _bridge.MessageReceived -= OnMessageAsync;
            _bridge.ConnectionChanged -= OnConnectionChangedAsync;
        }
    }

    private async Task OnConnectionChangedAsync(bool connected, CancellationToken cancellationToken)
    {
        if (!connected)
        {
            _capabilities.ReplaceBridgeCapabilities(Array.Empty<string>());
            _dispatcher.FailPending("The Sunrise native bridge disconnected.");
            Console.WriteLine("Sunrise native bridge disconnected.");
            return;
        }

        Console.WriteLine("Sunrise native bridge connected.");
        await _bridge.SendAsync(
            new { protocol = 1, type = "host.capabilities" },
            cancellationToken).ConfigureAwait(false);
        await _bridge.SendAsync(
            new { protocol = 1, type = "host.ping" },
            cancellationToken).ConfigureAwait(false);
    }

    private async Task OnMessageAsync(JsonElement message, CancellationToken cancellationToken)
    {
        if (!message.TryGetProperty("type", out JsonElement typeElement)
            || typeElement.ValueKind != JsonValueKind.String)
        {
            return;
        }

        string? type = typeElement.GetString();
        switch (type)
        {
            case "bridge.hello":
                HandleHello(message);
                await _runtime.TryResumeAsync(cancellationToken).ConfigureAwait(false);
                break;
            case "world.phase":
                await HandleWorldPhaseAsync(message, cancellationToken).ConfigureAwait(false);
                break;
            case "command.result":
                _dispatcher.Complete(message);
                break;
            case "scenario.signal":
                await HandleSignalAsync(message, cancellationToken).ConfigureAwait(false);
                break;
            case "bridge.pong":
                Console.WriteLine("Sunrise native bridge responded to ping.");
                break;
            case "placed-content.authority":
                HandlePlacedContentAuthority(message);
                break;
        }
    }

    private void HandleHello(JsonElement message)
    {
        var capabilities = new List<string>();
        if (message.TryGetProperty("capabilities", out JsonElement values)
            && values.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement value in values.EnumerateArray())
            {
                if (value.ValueKind == JsonValueKind.String && value.GetString() is { } capability)
                {
                    capabilities.Add(capability);
                }
            }
        }

        _capabilities.ReplaceBridgeCapabilities(capabilities);
        Console.WriteLine($"Bridge capabilities: {string.Join(", ", capabilities.OrderBy(item => item, StringComparer.Ordinal))}");
    }

    private async Task HandleSignalAsync(JsonElement message, CancellationToken cancellationToken)
    {
        if (!message.TryGetProperty("name", out JsonElement nameElement)
            || nameElement.ValueKind != JsonValueKind.String
            || nameElement.GetString() is not { } name)
        {
            return;
        }

        var fields = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["name"] = name,
        };
        await _runtime.PublishEventAsync(new HostEvent("signal", fields), cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task HandleWorldPhaseAsync(JsonElement message, CancellationToken cancellationToken)
    {
        string phase = message.TryGetProperty("phase", out JsonElement phaseElement)
            ? phaseElement.GetString() ?? "unknown"
            : "unknown";
        string transitionAge = message.TryGetProperty("transitionAgeMs", out JsonElement ageElement)
            ? ageElement.GetRawText()
            : "0";
        var fields = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["phase"] = phase,
            ["transitionAgeMs"] = transitionAge,
        };
        await _runtime.PublishEventAsync(new HostEvent("world.phase", fields), cancellationToken)
            .ConfigureAwait(false);
    }

    private static void HandlePlacedContentAuthority(JsonElement message)
    {
        if (!PlacedContentAuthorityObservation.TryParse(message, out PlacedContentAuthorityObservation? observation)
            || observation is null)
        {
            Console.Error.WriteLine("Rejected malformed placed-content.authority observation.");
            return;
        }

        Console.WriteLine(
            $"Placed-content authority: decodes={observation.DecodeCount}, "
            + $"forced-reads={observation.ForcedReadCount}, "
            + $"last-forced-reads={observation.LastDecoderForcedReads}, "
            + $"dropped={observation.DroppedCount}, "
            + $"last-decoder-succeeded={(observation.LastDecoderSucceeded ? "true" : "false")}.");
    }
}
