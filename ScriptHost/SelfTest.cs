using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Sunrise.ScriptHost.Protocol;
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

        VerifyBoundedUtf8LineBuffer();
        VerifyPlacedContentAuthorityParsing();
        await VerifyNamedPipeBridgeAsync().ConfigureAwait(false);

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

    private static void VerifyBoundedUtf8LineBuffer()
    {
        var buffer = new BoundedUtf8LineBuffer(maximumLineBytes: 2048);
        foreach (byte value in Encoding.UTF8.GetBytes("caf\u00e9"))
        {
            if (buffer.Push(value, out _) != BoundedLineResult.Pending)
            {
                throw new InvalidOperationException("Buffer completed a line before its newline.");
            }
        }
        if (buffer.Push((byte)'\n', out string? split) != BoundedLineResult.Complete
            || split != "caf\u00e9")
        {
            throw new InvalidOperationException("Buffer did not decode a split multi-byte line.");
        }

        var crlf = new BoundedUtf8LineBuffer(2048);
        foreach (byte value in Encoding.UTF8.GetBytes("hello"))
        {
            crlf.Push(value, out _);
        }
        crlf.Push((byte)'\r', out _);
        if (crlf.Push((byte)'\n', out string? crlfLine) != BoundedLineResult.Complete
            || crlfLine != "hello")
        {
            throw new InvalidOperationException("Buffer did not strip one terminal CR.");
        }

        var exact = new BoundedUtf8LineBuffer(2048);
        for (int index = 0; index < 2048; ++index)
        {
            if (exact.Push((byte)'a', out _) != BoundedLineResult.Pending)
            {
                throw new InvalidOperationException("Exact-capacity line completed early.");
            }
        }
        if (exact.Push((byte)'\n', out string? exactLine) != BoundedLineResult.Complete
            || exactLine is not { Length: 2048 })
        {
            throw new InvalidOperationException("Exact 2,048-byte line was not accepted.");
        }

        var oversized = new BoundedUtf8LineBuffer(2048);
        for (int index = 0; index < 2048; ++index)
        {
            oversized.Push((byte)'a', out _);
        }
        string? oversizedLine = "sentinel";
        if (oversized.Push((byte)'a', out oversizedLine) != BoundedLineResult.Oversized
            || oversizedLine is not null)
        {
            throw new InvalidOperationException("2,049-byte line was not rejected without a string.");
        }
        foreach (byte value in Encoding.UTF8.GetBytes("ok"))
        {
            oversized.Push(value, out _);
        }
        if (oversized.Push((byte)'\n', out string? afterOversized) != BoundedLineResult.Complete
            || afterOversized != "ok")
        {
            throw new InvalidOperationException("Buffer did not reset after Oversized.");
        }

        var invalid = new BoundedUtf8LineBuffer(2048);
        invalid.Push(0xFF, out _);
        if (invalid.Push((byte)'\n', out string? invalidLine) != BoundedLineResult.InvalidUtf8
            || invalidLine is not null)
        {
            throw new InvalidOperationException("Malformed UTF-8 was not rejected.");
        }
        foreach (byte value in Encoding.UTF8.GetBytes("ok"))
        {
            invalid.Push(value, out _);
        }
        if (invalid.Push((byte)'\n', out string? afterInvalid) != BoundedLineResult.Complete
            || afterInvalid != "ok")
        {
            throw new InvalidOperationException("Buffer did not reset after malformed UTF-8.");
        }
    }

    private static void VerifyPlacedContentAuthorityParsing()
    {
        string wire = JsonSerializer.Serialize(
            new
            {
                protocol = 1,
                type = "placed-content.authority",
                decodeCount = 1UL,
                forcedReadCount = 65UL,
                lastDecoderForcedReads = 65UL,
                droppedCount = 0UL,
                lastDecoderSucceeded = true,
            },
            Json.ProtocolOptions);

        if (wire.Contains('\r') || wire.Contains('\n'))
        {
            throw new InvalidOperationException(
                "Observation wire JSON must contain exactly one physical line.");
        }

        using JsonDocument document = JsonDocument.Parse(wire);
        if (!PlacedContentAuthorityObservation.TryParse(document.RootElement, out var observation)
            || observation is null
            || observation.DecodeCount != 1UL
            || observation.ForcedReadCount != 65UL
            || observation.LastDecoderForcedReads != 65UL
            || observation.DroppedCount != 0UL
            || !observation.LastDecoderSucceeded)
        {
            throw new InvalidOperationException("Representative observation did not parse exactly.");
        }

        AssertRejectsObservation(
            "{\"protocol\":1,\"type\":\"placed-content.authority\",\"forcedReadCount\":1,"
            + "\"lastDecoderForcedReads\":1,\"droppedCount\":1,\"lastDecoderSucceeded\":true}");
        AssertRejectsObservation(
            "{\"protocol\":1,\"type\":\"placed-content.authority\",\"decodeCount\":-1,"
            + "\"forcedReadCount\":1,\"lastDecoderForcedReads\":1,\"droppedCount\":1,"
            + "\"lastDecoderSucceeded\":true}");
        AssertRejectsObservation(
            "{\"protocol\":1,\"type\":\"placed-content.authority\",\"decodeCount\":18446744073709551616,"
            + "\"forcedReadCount\":1,\"lastDecoderForcedReads\":1,\"droppedCount\":1,"
            + "\"lastDecoderSucceeded\":true}");
        AssertRejectsObservation(
            "{\"protocol\":1,\"type\":\"placed-content.authority\",\"decodeCount\":1,"
            + "\"forcedReadCount\":1,\"lastDecoderForcedReads\":1,\"droppedCount\":1,"
            + "\"lastDecoderSucceeded\":1}");
    }

    private static void AssertRejectsObservation(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        if (PlacedContentAuthorityObservation.TryParse(document.RootElement, out _))
        {
            throw new InvalidOperationException("Malformed observation was accepted.");
        }
    }

    private static async Task VerifyNamedPipeBridgeAsync()
    {
        string pipeName = $"sunrise-script-host-self-test-{Guid.NewGuid():N}";
        await using var bridge = new NamedPipeBridge(pipeName);

        var messages = new List<string>();
        var states = new List<bool>();
        using var messageSignal = new SemaphoreSlim(0, int.MaxValue);
        using var stateSignal = new SemaphoreSlim(0, int.MaxValue);

        bridge.MessageReceived += (element, _) =>
        {
            lock (messages)
            {
                messages.Add(element.GetProperty("type").GetString() ?? string.Empty);
            }
            messageSignal.Release();
            return Task.CompletedTask;
        };
        bridge.ConnectionChanged += (connected, _) =>
        {
            lock (states)
            {
                states.Add(connected);
            }
            stateSignal.Release();
            return Task.CompletedTask;
        };

        using var cts = new CancellationTokenSource();
        Task bridgeTask = bridge.RunAsync(cts.Token);

        try
        {
            await using (NamedPipeClientStream client =
                await ConnectWithRetryAsync(pipeName, cts.Token).ConfigureAwait(false))
            {
                await WaitForStateCountAsync(states, stateSignal, 1).ConfigureAwait(false);
                if (states[0] != true)
                {
                    throw new InvalidOperationException("Bridge did not report the first connection.");
                }

                byte[] split = Encoding.UTF8.GetBytes("{\"protocol\":1,\"type\":\"host.ping\"}\n");
                await client.WriteAsync(split.AsMemory(0, split.Length / 2), cts.Token)
                    .ConfigureAwait(false);
                await client.FlushAsync(cts.Token).ConfigureAwait(false);
                await client.WriteAsync(split.AsMemory(split.Length / 2), cts.Token)
                    .ConfigureAwait(false);
                await client.FlushAsync(cts.Token).ConfigureAwait(false);
                await WaitForMessageCountAsync(messages, messageSignal, 1).ConfigureAwait(false);
                if (messages[0] != "host.ping")
                {
                    throw new InvalidOperationException("Split line was not delivered.");
                }

                await SendLineAsync(client, "{malformed", cts.Token).ConfigureAwait(false);
                await SendLineAsync(client, "{\"protocol\":1,\"type\":\"bridge.pong\"}", cts.Token)
                    .ConfigureAwait(false);
                await WaitForMessageCountAsync(messages, messageSignal, 2).ConfigureAwait(false);
                if (messages[1] != "bridge.pong")
                {
                    throw new InvalidOperationException("Connection did not survive malformed JSON.");
                }
                if (states.Any(state => !state))
                {
                    throw new InvalidOperationException("Malformed JSON disconnected the bridge.");
                }

                byte[] oversized = new byte[2049];
                Array.Fill(oversized, (byte)'a');
                try
                {
                    await client.WriteAsync(oversized, cts.Token).ConfigureAwait(false);
                    await client.FlushAsync(cts.Token).ConfigureAwait(false);
                }
                catch (IOException)
                {
                }

                await WaitForStateCountAsync(states, stateSignal, 2).ConfigureAwait(false);
                if (states[1] != false)
                {
                    throw new InvalidOperationException("Oversized line did not disconnect the bridge.");
                }
            }

            await using (NamedPipeClientStream second =
                await ConnectWithRetryAsync(pipeName, cts.Token).ConfigureAwait(false))
            {
                await WaitForStateCountAsync(states, stateSignal, 3).ConfigureAwait(false);
                if (states[2] != true)
                {
                    throw new InvalidOperationException("Bridge did not report the reconnection.");
                }

                await SendLineAsync(second, "{\"protocol\":1,\"type\":\"host.ping\"}", cts.Token)
                    .ConfigureAwait(false);
                await WaitForMessageCountAsync(messages, messageSignal, 3).ConfigureAwait(false);
                if (messages[2] != "host.ping")
                {
                    throw new InvalidOperationException("Reconnected client did not receive a line.");
                }
            }
        }
        finally
        {
            cts.Cancel();
            await bridgeTask.ConfigureAwait(false);
        }
    }

    private static async Task<NamedPipeClientStream> ConnectWithRetryAsync(
        string pipeName,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (true)
        {
            var client = new NamedPipeClientStream(
                ".",
                pipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            try
            {
                await client.ConnectAsync(cancellationToken).ConfigureAwait(false);
                return client;
            }
            catch (IOException) when (DateTime.UtcNow < deadline)
            {
                await client.DisposeAsync().ConfigureAwait(false);
                await Task.Delay(50, cancellationToken).ConfigureAwait(false);
            }
            catch (TimeoutException) when (DateTime.UtcNow < deadline)
            {
                await client.DisposeAsync().ConfigureAwait(false);
                await Task.Delay(50, cancellationToken).ConfigureAwait(false);
            }
        }
    }

    private static async Task SendLineAsync(
        Stream stream,
        string line,
        CancellationToken cancellationToken)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(line + "\n");
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async Task WaitForMessageCountAsync(
        List<string> messages,
        SemaphoreSlim signal,
        int count)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            int current;
            lock (messages)
            {
                current = messages.Count;
            }
            if (current >= count)
            {
                return;
            }
            await signal.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false);
        }
        throw new InvalidOperationException($"Timed out waiting for {count} bridge messages.");
    }

    private static async Task WaitForStateCountAsync(
        List<bool> states,
        SemaphoreSlim signal,
        int count)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            int current;
            lock (states)
            {
                current = states.Count;
            }
            if (current >= count)
            {
                return;
            }
            await signal.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false);
        }
        throw new InvalidOperationException($"Timed out waiting for {count} bridge connection states.");
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
