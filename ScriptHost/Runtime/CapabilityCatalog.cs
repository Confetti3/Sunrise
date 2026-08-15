using System.Text.Json;

namespace Sunrise.ScriptHost.Runtime;

public enum CapabilityState
{
    Available,
    ProbeRequired,
    WireAdapterRequired,
    ClientPatchRequired,
    AuthoredPolicyRequired,
    Unsupported,
}

public sealed record CapabilityDefinition
{
    public required string Id { get; init; }
    public required CapabilityState Status { get; init; }
    public required string Owner { get; init; }
    public required string Evidence { get; init; }
    public string? Notes { get; init; }
}

public sealed record CapabilityManifest
{
    public int Schema { get; init; }
    public required string Build { get; init; }
    public required IReadOnlyList<CapabilityDefinition> Capabilities { get; init; }
}

public sealed record CapabilityDecision(
    string Id,
    bool Available,
    CapabilityState DeclaredState,
    string Owner,
    string Evidence,
    string? Notes);

public sealed class CapabilityCatalog
{
    private readonly Dictionary<string, CapabilityDefinition> _definitions;
    private readonly HashSet<string> _bridgeCapabilities = new(StringComparer.Ordinal);
    private readonly HashSet<string> _pluginCapabilities = new(StringComparer.Ordinal);
    private readonly object _gate = new();

    private CapabilityCatalog(CapabilityManifest manifest)
    {
        Build = manifest.Build;
        _definitions = manifest.Capabilities.ToDictionary(item => item.Id, StringComparer.Ordinal);
    }

    public string Build { get; }

    public static CapabilityCatalog Load(string path)
    {
        string json = File.ReadAllText(path);
        CapabilityManifest? manifest = JsonSerializer.Deserialize<CapabilityManifest>(json, Json.DefaultOptions);
        if (manifest is null)
        {
            throw new InvalidDataException($"Capability manifest is empty: {path}");
        }

        Validate(manifest, path);
        return new CapabilityCatalog(manifest);
    }

    public void ReplaceBridgeCapabilities(IEnumerable<string> capabilities)
    {
        lock (_gate)
        {
            _bridgeCapabilities.Clear();
            _bridgeCapabilities.UnionWith(
                capabilities.Where(item => !string.IsNullOrWhiteSpace(item)));
        }
    }

    public void ReplacePluginCapabilities(IEnumerable<string> capabilities)
    {
        lock (_gate)
        {
            _pluginCapabilities.Clear();
            _pluginCapabilities.UnionWith(
                capabilities.Where(item => !string.IsNullOrWhiteSpace(item)));
        }
    }

    public CapabilityDecision Decide(string id)
    {
        lock (_gate)
        {
            return DecideLocked(id);
        }
    }

    public IReadOnlyList<CapabilityDecision> Snapshot()
    {
        lock (_gate)
        {
            return _definitions.Keys
                .OrderBy(item => item, StringComparer.Ordinal)
                .Select(DecideLocked)
                .ToArray();
        }
    }

    private CapabilityDecision DecideLocked(string id)
    {
        bool runtimeAvailable = _bridgeCapabilities.Contains(id) || _pluginCapabilities.Contains(id);
        if (_definitions.TryGetValue(id, out CapabilityDefinition? definition))
        {
            return new CapabilityDecision(
                id,
                runtimeAvailable,
                definition.Status,
                definition.Owner,
                definition.Evidence,
                definition.Notes);
        }

        return new CapabilityDecision(
            id,
            runtimeAvailable,
            CapabilityState.Unsupported,
            "unclassified",
            "No build binding declares this capability.",
            null);
    }

    private static void Validate(CapabilityManifest manifest, string path)
    {
        if (manifest.Schema != 1)
        {
            throw new InvalidDataException($"Unsupported capability manifest schema {manifest.Schema}: {path}");
        }

        if (string.IsNullOrWhiteSpace(manifest.Build))
        {
            throw new InvalidDataException($"Capability manifest build is missing: {path}");
        }

        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (CapabilityDefinition definition in manifest.Capabilities)
        {
            if (string.IsNullOrWhiteSpace(definition.Id) || !ids.Add(definition.Id))
            {
                throw new InvalidDataException($"Capability ids must be nonempty and unique: {path}");
            }
        }
    }
}
