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
    private int _startRequested;
    private int _startPublished;
    private int _resumeRequested;

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

        Task bridgeTask = _bridge.RunAsync(cancellationToken);
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(100));
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                if (Interlocked.Exchange(ref _startRequested, 0) != 0
                    && Interlocked.Exchange(ref _startPublished, 1) == 0)
                {
                    // A host.start command may target the bridge, whose receive loop is already
                    // running on bridgeTask by the time this service-loop dispatch begins.
                    await _runtime.PublishEventAsync(HostEvent.Start, cancellationToken)
                        .ConfigureAwait(false);
                }
                await _runtime.TickAsync(cancellationToken).ConfigureAwait(false);
                if (Interlocked.Exchange(ref _resumeRequested, 0) != 0)
                {
                    await _runtime.TryResumeAsync(cancellationToken).ConfigureAwait(false);
                }
            }
        }
        finally
        {
            await bridgeTask.ConfigureAwait(false);
            _bridge.MessageReceived -= OnMessageAsync;
            _bridge.ConnectionChanged -= OnConnectionChangedAsync;
        }
    }

    private Task OnConnectionChangedAsync(bool connected, CancellationToken cancellationToken)
    {
        if (!connected)
        {
            _capabilities.ReplaceBridgeCapabilities(Array.Empty<string>());
            _dispatcher.FailPending("The Sunrise native bridge disconnected.");
            Console.WriteLine("Sunrise native bridge disconnected.");
            return Task.CompletedTask;
        }

        Console.WriteLine("Sunrise native bridge connected.");
        return Task.CompletedTask;
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
                // The receive loop must return before a command dispatched by TryResumeAsync can
                // receive its command.result. The service loop consumes this one-shot request.
                Interlocked.Exchange(ref _startRequested, 1);
                Interlocked.Exchange(ref _resumeRequested, 1);
                break;
            case "world.phase":
                await HandleWorldPhaseAsync(message, cancellationToken).ConfigureAwait(false);
                break;
            case "activity.incident":
                await HandleActivityIncidentAsync(message, cancellationToken).ConfigureAwait(false);
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
        Console.WriteLine($"World phase observed: phase={phase} transitionAgeMs={transitionAge}");
        await _runtime.PublishEventAsync(new HostEvent("world.phase", fields), cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task HandleActivityIncidentAsync(
        JsonElement message,
        CancellationToken cancellationToken)
    {
        if (!message.TryGetProperty("primaryTarget", out JsonElement targetElement)
            || targetElement.ValueKind != JsonValueKind.Number
            || !targetElement.TryGetUInt32(out uint primaryTarget))
        {
            Console.Error.WriteLine("Ignored malformed activity.incident without a uint primaryTarget.");
            return;
        }

        var fields = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["primaryTarget"] = primaryTarget.ToString(System.Globalization.CultureInfo.InvariantCulture),
        };
        CopyScalarField(message, fields, "sequence");
        CopyScalarField(message, fields, "observedAtTickMs");
        CopyScalarField(message, fields, "sessionId");
        CopyScalarField(message, fields, "accountHandle");
        CopyScalarField(message, fields, "extraTargetCount");
        CopyScalarField(message, fields, "payloadLength");
        CopyScalarField(message, fields, "hasCompressedSelector");
        CopyScalarField(message, fields, "hasPayload");
        CopyScalarField(message, fields, "droppedBefore");

        Console.WriteLine(
            $"Activity incident observed: target={fields["primaryTarget"]} sequence={fields.GetValueOrDefault("sequence", "unknown")}");
        await _runtime.PublishEventAsync(
                new HostEvent("activity.incident", fields, message.Clone()),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static void CopyScalarField(
        JsonElement message,
        IDictionary<string, string> fields,
        string name)
    {
        if (!message.TryGetProperty(name, out JsonElement value))
        {
            return;
        }

        switch (value.ValueKind)
        {
            case JsonValueKind.String when value.GetString() is { } text:
                fields[name] = text;
                break;
            case JsonValueKind.Number:
            case JsonValueKind.True:
            case JsonValueKind.False:
                fields[name] = value.GetRawText();
                break;
        }
    }
}
