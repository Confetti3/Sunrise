using System.Text.Json;

namespace Sunrise.ScriptHost.Runtime;

public sealed record MissionCheckpoint
{
    public required string ScenarioId { get; init; }
    public required string CurrentNodeId { get; init; }
    public int ActionIndex { get; init; }
    public bool Triggered { get; init; }
    public DateTimeOffset NodeEnteredAtUtc { get; init; }
    public string? PausedReason { get; init; }
    public bool Completed { get; init; }
    public Dictionary<string, string> Variables { get; init; } = new(StringComparer.Ordinal);
}

public sealed class CheckpointStore
{
    private readonly string _path;

    public CheckpointStore(string path)
    {
        _path = path;
    }

    public async Task<MissionCheckpoint?> LoadAsync(CancellationToken cancellationToken)
    {
        if (!File.Exists(_path))
        {
            return null;
        }

        await using FileStream stream = File.OpenRead(_path);
        return await JsonSerializer.DeserializeAsync<MissionCheckpoint>(
            stream,
            Json.DefaultOptions,
            cancellationToken).ConfigureAwait(false);
    }

    public async Task SaveAsync(MissionCheckpoint checkpoint, CancellationToken cancellationToken)
    {
        string? directory = Path.GetDirectoryName(_path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        string temporaryPath = _path + ".tmp";
        await using (FileStream stream = File.Create(temporaryPath))
        {
            await JsonSerializer.SerializeAsync(
                stream,
                checkpoint,
                Json.DefaultOptions,
                cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        }

        File.Move(temporaryPath, _path, overwrite: true);
    }
}
