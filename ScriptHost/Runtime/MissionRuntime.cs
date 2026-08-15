using System.Text.Json;

namespace Sunrise.ScriptHost.Runtime;

public sealed class MissionRuntime
{
    private readonly ScenarioDefinition _scenario;
    private readonly Dictionary<string, ScenarioNode> _nodes;
    private readonly CapabilityCatalog _capabilities;
    private readonly ICommandDispatcher _dispatcher;
    private readonly CheckpointStore _checkpointStore;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private MissionCheckpoint? _checkpoint;

    public MissionRuntime(
        ScenarioDefinition scenario,
        CapabilityCatalog capabilities,
        ICommandDispatcher dispatcher,
        CheckpointStore checkpointStore)
    {
        _scenario = scenario;
        _nodes = scenario.Nodes.ToDictionary(node => node.Id, StringComparer.Ordinal);
        _capabilities = capabilities;
        _dispatcher = dispatcher;
        _checkpointStore = checkpointStore;
    }

    public bool IsCompleted => _checkpoint?.Completed == true;
    public string? PausedReason => _checkpoint?.PausedReason;

    public async Task InitializeAsync(CancellationToken cancellationToken)
    {
        MissionCheckpoint? saved = await _checkpointStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (saved is not null
            && string.Equals(saved.ScenarioId, _scenario.Id, StringComparison.Ordinal)
            && _nodes.ContainsKey(saved.CurrentNodeId))
        {
            _checkpoint = saved;
            Console.WriteLine($"Resumed scenario '{_scenario.Id}' at node '{saved.CurrentNodeId}'.");
            return;
        }

        _checkpoint = new MissionCheckpoint
        {
            ScenarioId = _scenario.Id,
            CurrentNodeId = _scenario.Start,
            ActionIndex = 0,
            Triggered = false,
            NodeEnteredAtUtc = DateTimeOffset.UtcNow,
            PausedReason = null,
            Completed = false,
            Variables = new Dictionary<string, string>(_scenario.Parameters, StringComparer.Ordinal),
        };
        await SaveAsync(cancellationToken).ConfigureAwait(false);
    }

    public async Task PublishEventAsync(HostEvent hostEvent, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            EnsureInitialized();
            if (_checkpoint!.Completed)
            {
                return;
            }

            ScenarioNode node = _nodes[_checkpoint.CurrentNodeId];
            if (!_checkpoint.Triggered && TriggerMatches(node.Trigger, hostEvent, _checkpoint.NodeEnteredAtUtc))
            {
                _checkpoint = _checkpoint with { Triggered = true, PausedReason = null };
                await SaveAsync(cancellationToken).ConfigureAwait(false);
                await ExecuteAndAdvanceAsync(cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task TickAsync(CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            EnsureInitialized();
            if (_checkpoint!.Completed)
            {
                return;
            }

            ScenarioNode node = _nodes[_checkpoint.CurrentNodeId];
            if (!_checkpoint.Triggered
                && TriggerMatches(
                    node.Trigger,
                    new HostEvent("host.tick", new Dictionary<string, string>(StringComparer.Ordinal)),
                    _checkpoint.NodeEnteredAtUtc))
            {
                _checkpoint = _checkpoint with { Triggered = true, PausedReason = null };
                await SaveAsync(cancellationToken).ConfigureAwait(false);
                await ExecuteAndAdvanceAsync(cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task TryResumeAsync(CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            EnsureInitialized();
            if (_checkpoint!.Completed || !_checkpoint.Triggered || _checkpoint.PausedReason is null)
            {
                return;
            }

            _checkpoint = _checkpoint with { PausedReason = null };
            await ExecuteAndAdvanceAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task ExecuteAndAdvanceAsync(CancellationToken cancellationToken)
    {
        while (!_checkpoint!.Completed && _checkpoint.Triggered)
        {
            ScenarioNode node = _nodes[_checkpoint.CurrentNodeId];
            while (_checkpoint.ActionIndex < node.Actions.Count)
            {
                ActionDefinition action = node.Actions[_checkpoint.ActionIndex];
                bool completed = await ExecuteActionAsync(action, cancellationToken).ConfigureAwait(false);
                if (!completed)
                {
                    await SaveAsync(cancellationToken).ConfigureAwait(false);
                    return;
                }

                _checkpoint = _checkpoint with
                {
                    ActionIndex = _checkpoint.ActionIndex + 1,
                    PausedReason = null,
                };
                await SaveAsync(cancellationToken).ConfigureAwait(false);
            }

            if (node.Next is null)
            {
                _checkpoint = _checkpoint with { Completed = true, PausedReason = null };
                Console.WriteLine($"Scenario '{_scenario.Id}' completed.");
                await SaveAsync(cancellationToken).ConfigureAwait(false);
                return;
            }

            _checkpoint = _checkpoint with
            {
                CurrentNodeId = node.Next,
                ActionIndex = 0,
                Triggered = false,
                NodeEnteredAtUtc = DateTimeOffset.UtcNow,
                PausedReason = null,
            };
            Console.WriteLine($"Scenario node: {_checkpoint.CurrentNodeId}");
            await SaveAsync(cancellationToken).ConfigureAwait(false);

            ScenarioNode next = _nodes[_checkpoint.CurrentNodeId];
            if (!string.Equals(next.Trigger.Type, "immediate", StringComparison.Ordinal))
            {
                return;
            }

            _checkpoint = _checkpoint with { Triggered = true };
        }
    }

    private async Task<bool> ExecuteActionAsync(ActionDefinition action, CancellationToken cancellationToken)
    {
        switch (action.Type)
        {
            case "log":
                Console.WriteLine(TemplateExpander.Expand(action.Message ?? string.Empty, _checkpoint!.Variables));
                return true;
            case "set":
                SetVariable(action);
                return true;
            case "signal":
                Console.WriteLine($"Signal emitted: {action.Name}");
                return true;
            case "command":
                return await ExecuteCommandAsync(action, cancellationToken).ConfigureAwait(false);
            default:
                throw new InvalidOperationException($"Action type '{action.Type}' passed validation but has no executor.");
        }
    }

    private void SetVariable(ActionDefinition action)
    {
        JsonElement value = action.Value!.Value;
        string text = value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : value.GetRawText();
        _checkpoint!.Variables[action.Name!] = TemplateExpander.Expand(text, _checkpoint.Variables);
    }

    private async Task<bool> ExecuteCommandAsync(ActionDefinition action, CancellationToken cancellationToken)
    {
        string capability = action.Capability!;
        CapabilityDecision decision = _capabilities.Decide(capability);
        if (!decision.Available)
        {
            string reason = $"Missing capability '{capability}': {decision.DeclaredState} ({decision.Evidence})";
            _checkpoint = _checkpoint! with { PausedReason = reason };
            Console.Error.WriteLine(reason);
            return false;
        }

        JsonElement payload = TemplateExpander.Expand(action.Payload, _checkpoint!.Variables);
        CommandResult result = await _dispatcher.DispatchAsync(
            capability,
            payload,
            _checkpoint.Variables,
            cancellationToken).ConfigureAwait(false);
        if (result.Success)
        {
            return true;
        }

        string failure = $"Command '{capability}' failed with status '{result.Status}': {result.Reason}";
        _checkpoint = _checkpoint with { PausedReason = failure };
        Console.Error.WriteLine(failure);
        return false;
    }

    private static bool TriggerMatches(
        TriggerDefinition trigger,
        HostEvent hostEvent,
        DateTimeOffset nodeEnteredAtUtc)
    {
        return trigger.Type switch
        {
            "host.start" => string.Equals(hostEvent.Type, "host.start", StringComparison.Ordinal),
            "immediate" => true,
            "delay" => DateTimeOffset.UtcNow - nodeEnteredAtUtc
                >= TimeSpan.FromMilliseconds(trigger.Milliseconds ?? 0),
            "signal" => string.Equals(hostEvent.Type, "signal", StringComparison.Ordinal)
                && hostEvent.Fields.TryGetValue("name", out string? name)
                && string.Equals(name, trigger.Value, StringComparison.Ordinal),
            "world.phase" => string.Equals(hostEvent.Type, "world.phase", StringComparison.Ordinal)
                && hostEvent.Fields.TryGetValue("phase", out string? phase)
                && string.Equals(phase, trigger.Value, StringComparison.Ordinal),
            _ => false,
        };
    }

    private async Task SaveAsync(CancellationToken cancellationToken)
    {
        await _checkpointStore.SaveAsync(_checkpoint!, cancellationToken).ConfigureAwait(false);
    }

    private void EnsureInitialized()
    {
        if (_checkpoint is null)
        {
            throw new InvalidOperationException("Mission runtime was used before InitializeAsync.");
        }
    }
}
