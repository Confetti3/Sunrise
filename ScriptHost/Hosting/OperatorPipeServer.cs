using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Sunrise.ScriptHost.Protocol;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost.Hosting;

internal sealed record OperatorReply(bool Ok, string Status, string? Reason, object? Result = null);

internal sealed class OperatorPipeServer
{
    private const int MaximumLineBytes = 4_096;
    private const int MaximumLogLines = 500;
    private static readonly IReadOnlyDictionary<string, string> EmptyVariables =
        new Dictionary<string, string>(StringComparer.Ordinal);

    private readonly string _logPath;
    private readonly NamedPipeBridge _bridge;
    private readonly CapabilityCatalog _capabilities;
    private readonly MissionRuntime _runtime;
    private readonly ICommandDispatcher _dispatcher;

    internal OperatorPipeServer(
        string bridgePipeName,
        string? logPath,
        NamedPipeBridge bridge,
        CapabilityCatalog capabilities,
        MissionRuntime runtime,
        ICommandDispatcher dispatcher)
    {
        PipeName = bridgePipeName + "-operator";
        _logPath = logPath ?? string.Empty;
        _bridge = bridge;
        _capabilities = capabilities;
        _runtime = runtime;
        _dispatcher = dispatcher;
    }

    internal string PipeName { get; }

    internal async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                PipeName,
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            await pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                await HandleConnectionAsync(pipe, cancellationToken).ConfigureAwait(false);
            }
            catch (IOException exception)
            {
                Console.Error.WriteLine($"Operator pipe connection failed: {exception.Message}");
            }
        }
    }

    private async Task HandleConnectionAsync(
        NamedPipeServerStream pipe,
        CancellationToken cancellationToken)
    {
        var lineBuffer = new BoundedUtf8LineBuffer(MaximumLineBytes);
        byte[] readBuffer = new byte[512];
        while (pipe.IsConnected)
        {
            int count = await pipe.ReadAsync(readBuffer, cancellationToken).ConfigureAwait(false);
            if (count == 0)
            {
                return;
            }
            for (int index = 0; index < count; ++index)
            {
                BoundedLineResult result = lineBuffer.Push(readBuffer[index], out string? line);
                if (result == BoundedLineResult.Pending)
                {
                    continue;
                }
                if (result != BoundedLineResult.Complete || line is null)
                {
                    await WriteReplyAsync(
                            pipe,
                            new OperatorReply(false, "invalid-request", "Request must be one bounded UTF-8 line."),
                            cancellationToken)
                        .ConfigureAwait(false);
                    return;
                }

                OperatorReply reply = await ExecuteAsync(line, cancellationToken).ConfigureAwait(false);
                await WriteReplyAsync(pipe, reply, cancellationToken).ConfigureAwait(false);
                return;
            }
        }
    }

    private async Task<OperatorReply> ExecuteAsync(
        string line,
        CancellationToken cancellationToken)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(line);
        }
        catch (JsonException exception)
        {
            return new OperatorReply(false, "invalid-json", exception.Message);
        }

        using (document)
        {
            JsonElement request = document.RootElement;
            if (request.ValueKind != JsonValueKind.Object
                || !request.TryGetProperty("protocol", out JsonElement protocol)
                || !protocol.TryGetInt32(out int version)
                || version != 1
                || !TryString(request, "command", out string? command))
            {
                return new OperatorReply(false, "invalid-request", "protocol 1 and a string command are required.");
            }

            return command switch
            {
                "runtime.status" => await RuntimeStatusAsync(cancellationToken).ConfigureAwait(false),
                "activity.snapshot" => await ActivitySnapshotAsync(cancellationToken).ConfigureAwait(false),
                "gameplay-switch.read" => await GameplaySwitchAsync(request, write: false, cancellationToken).ConfigureAwait(false),
                "gameplay-switch.set" => await GameplaySwitchAsync(request, write: true, cancellationToken).ConfigureAwait(false),
                "placed-content.authority.observe" => await DispatchAsync(
                        "placed-content.authority.observe",
                        new { },
                        cancellationToken)
                    .ConfigureAwait(false),
                "scenario.status" => await ScenarioStatusAsync(cancellationToken).ConfigureAwait(false),
                "scenario.reset" => await ScenarioResetAsync(cancellationToken).ConfigureAwait(false),
                "scenario.signal" => await ScenarioSignalAsync(request, cancellationToken).ConfigureAwait(false),
                "logs.tail" => await LogsTailAsync(request, cancellationToken).ConfigureAwait(false),
                _ => new OperatorReply(false, "unsupported", $"Unknown operator command '{command}'."),
            };
        }
    }

    private async Task<OperatorReply> RuntimeStatusAsync(CancellationToken cancellationToken)
    {
        MissionRuntimeStatus scenario = await _runtime.GetStatusAsync(cancellationToken).ConfigureAwait(false);
        string[] capabilities = _capabilities.Snapshot()
            .Where(item => item.Available)
            .Select(item => item.Id)
            .OrderBy(item => item, StringComparer.Ordinal)
            .ToArray();
        return new OperatorReply(
            true,
            "ok",
            null,
            new
            {
                bridgeConnected = _bridge.IsConnected,
                build = _capabilities.Build,
                capabilities,
                scenario,
            });
    }

    private async Task<OperatorReply> ScenarioStatusAsync(CancellationToken cancellationToken)
    {
        MissionRuntimeStatus status = await _runtime.GetStatusAsync(cancellationToken).ConfigureAwait(false);
        return new OperatorReply(true, "ok", null, status);
    }

    private async Task<OperatorReply> ScenarioResetAsync(CancellationToken cancellationToken)
    {
        await _runtime.ResetAsync(cancellationToken).ConfigureAwait(false);
        await _runtime.PublishEventAsync(HostEvent.Start, cancellationToken).ConfigureAwait(false);
        MissionRuntimeStatus status = await _runtime.GetStatusAsync(cancellationToken).ConfigureAwait(false);
        return new OperatorReply(true, "ok", null, status);
    }

    private async Task<OperatorReply> ScenarioSignalAsync(
        JsonElement request,
        CancellationToken cancellationToken)
    {
        if (!TryString(request, "name", out string? name)
            || name is null
            || !ValidToken(name))
        {
            return new OperatorReply(false, "invalid-request", "name must be a bounded signal token.");
        }
        var fields = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["name"] = name,
        };
        await _runtime.PublishEventAsync(new HostEvent("signal", fields), cancellationToken)
            .ConfigureAwait(false);
        return new OperatorReply(true, "ok", null, new { name });
    }

    private async Task<OperatorReply> ActivitySnapshotAsync(CancellationToken cancellationToken)
    {
        OperatorReply reply = await DispatchAsync("activity.snapshot", new { }, cancellationToken)
            .ConfigureAwait(false);
        if (!reply.Ok || reply.Result is not JsonElement result)
        {
            return reply;
        }
        if (!ActivitySnapshot.TryParse(result, out ActivitySnapshot? snapshot) || snapshot is null)
        {
            return new OperatorReply(false, "invalid-response", "Native activity.snapshot returned an invalid payload.");
        }
        return new OperatorReply(true, "ok", null, snapshot);
    }

    private async Task<OperatorReply> GameplaySwitchAsync(
        JsonElement request,
        bool write,
        CancellationToken cancellationToken)
    {
        if (!TryUInt16(request, "definitionIndex", out ushort definitionIndex))
        {
            return new OperatorReply(false, "invalid-request", "definitionIndex must be a UInt16.");
        }
        if (!write)
        {
            return await DispatchAsync(
                    "gameplay-switch.read",
                    new { definitionIndex },
                    cancellationToken)
                .ConfigureAwait(false);
        }
        if (!request.TryGetProperty("value", out JsonElement valueElement)
            || !valueElement.TryGetInt32(out int value)
            || value is < 0 or > 2)
        {
            return new OperatorReply(false, "invalid-request", "value must be 0, 1, or 2.");
        }
        return await DispatchAsync(
                "gameplay-switch.set",
                new { definitionIndex, value },
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task<OperatorReply> DispatchAsync<T>(
        string capability,
        T payload,
        CancellationToken cancellationToken)
    {
        using JsonDocument document = JsonSerializer.SerializeToDocument(payload, Json.ProtocolOptions);
        CommandResult result = await _dispatcher.DispatchAsync(
                capability,
                document.RootElement,
                EmptyVariables,
                cancellationToken)
            .ConfigureAwait(false);
        return new OperatorReply(result.Success, result.Status, result.Reason, result.Result);
    }

    private async Task<OperatorReply> LogsTailAsync(
        JsonElement request,
        CancellationToken cancellationToken)
    {
        int last = 50;
        if (request.TryGetProperty("last", out JsonElement lastElement)
            && (!lastElement.TryGetInt32(out last) || last is < 1 or > MaximumLogLines))
        {
            return new OperatorReply(false, "invalid-request", $"last must be between 1 and {MaximumLogLines}.");
        }
        if (string.IsNullOrEmpty(_logPath) || !File.Exists(_logPath))
        {
            return new OperatorReply(false, "not-found", "The configured Sunrise log does not exist.");
        }

        var lines = new Queue<string>(last);
        await using var stream = new FileStream(
            _logPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete,
            bufferSize: 4_096,
            useAsync: true);
        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        while (await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false) is { } line)
        {
            if (lines.Count == last)
            {
                lines.Dequeue();
            }
            lines.Enqueue(line);
        }
        return new OperatorReply(true, "ok", null, new { path = _logPath, lines = lines.ToArray() });
    }

    private static async Task WriteReplyAsync(
        NamedPipeServerStream pipe,
        OperatorReply reply,
        CancellationToken cancellationToken)
    {
        string line = JsonSerializer.Serialize(reply, Json.ProtocolOptions) + "\n";
        byte[] bytes = Encoding.UTF8.GetBytes(line);
        await pipe.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await pipe.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    private static bool TryString(JsonElement value, string name, out string? result)
    {
        result = null;
        return value.TryGetProperty(name, out JsonElement property)
               && property.ValueKind == JsonValueKind.String
               && (result = property.GetString()) is not null;
    }

    private static bool TryUInt16(JsonElement value, string name, out ushort result)
    {
        result = 0;
        return value.TryGetProperty(name, out JsonElement property)
               && property.ValueKind == JsonValueKind.Number
               && property.TryGetUInt16(out result);
    }

    private static bool ValidToken(string value) =>
        value.Length is > 0 and <= 96
        && value.All(character => char.IsAsciiLetterOrDigit(character)
                                  || character is '-' or '_' or '.');
}
