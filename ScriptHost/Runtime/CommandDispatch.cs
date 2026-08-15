using System.Collections.Concurrent;
using System.Text.Json;
using Sunrise.ScriptHost.Plugins;
using Sunrise.ScriptHost.Protocol;

namespace Sunrise.ScriptHost.Runtime;

public sealed record CommandResult(bool Success, string Status, string? Reason, JsonElement? Result = null);

public interface ICommandDispatcher
{
    Task<CommandResult> DispatchAsync(
        string capability,
        JsonElement payload,
        IReadOnlyDictionary<string, string> variables,
        CancellationToken cancellationToken);
}

public sealed class CompositeCommandDispatcher : ICommandDispatcher
{
    private static readonly TimeSpan CommandTimeout = TimeSpan.FromSeconds(10);
    private readonly NamedPipeBridge _bridge;
    private readonly PluginRegistry _plugins;
    private readonly ConcurrentDictionary<string, TaskCompletionSource<CommandResult>> _pending = new(StringComparer.Ordinal);

    public CompositeCommandDispatcher(NamedPipeBridge bridge, PluginRegistry plugins)
    {
        _bridge = bridge;
        _plugins = plugins;
    }

    public async Task<CommandResult> DispatchAsync(
        string capability,
        JsonElement payload,
        IReadOnlyDictionary<string, string> variables,
        CancellationToken cancellationToken)
    {
        if (_plugins.TryGet(capability, out IHostPlugin? plugin) && plugin is not null)
        {
            PluginCommandResult result = await plugin.ExecuteAsync(
                new PluginCommandContext(capability, payload, variables),
                cancellationToken).ConfigureAwait(false);
            return new CommandResult(result.Success, result.Success ? "ok" : "plugin-error", result.Reason, result.Result);
        }

        if (!_bridge.IsConnected)
        {
            return new CommandResult(false, "disconnected", "The Sunrise native bridge is not connected.");
        }

        string requestId = Guid.NewGuid().ToString("D");
        var completion = new TaskCompletionSource<CommandResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_pending.TryAdd(requestId, completion))
        {
            return new CommandResult(false, "internal-error", "Could not allocate a unique request id.");
        }

        try
        {
            bool sent = await _bridge.SendAsync(
                new
                {
                    protocol = 1,
                    type = "command.request",
                    requestId,
                    capability,
                    payload,
                },
                cancellationToken).ConfigureAwait(false);
            if (!sent)
            {
                return new CommandResult(false, "disconnected", "The bridge disconnected before the command was sent.");
            }

            return await completion.Task.WaitAsync(CommandTimeout, cancellationToken).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            return new CommandResult(false, "timeout", $"The bridge did not answer '{capability}' within {CommandTimeout.TotalSeconds:F0} seconds.");
        }
        finally
        {
            _pending.TryRemove(requestId, out _);
        }
    }

    public void Complete(JsonElement message)
    {
        if (!message.TryGetProperty("requestId", out JsonElement requestIdElement)
            || requestIdElement.ValueKind != JsonValueKind.String)
        {
            return;
        }

        string? requestId = requestIdElement.GetString();
        if (requestId is null || !_pending.TryGetValue(requestId, out TaskCompletionSource<CommandResult>? completion))
        {
            return;
        }

        string status = message.TryGetProperty("status", out JsonElement statusElement)
            ? statusElement.GetString() ?? "unknown"
            : "unknown";
        string? reason = message.TryGetProperty("reason", out JsonElement reasonElement)
            ? reasonElement.GetString()
            : null;
        JsonElement? result = message.TryGetProperty("result", out JsonElement resultElement)
            ? resultElement.Clone()
            : null;
        completion.TrySetResult(new CommandResult(
            string.Equals(status, "ok", StringComparison.Ordinal),
            status,
            reason,
            result));
    }

    public void FailPending(string reason)
    {
        foreach (TaskCompletionSource<CommandResult> completion in _pending.Values)
        {
            completion.TrySetResult(new CommandResult(false, "disconnected", reason));
        }
    }
}
