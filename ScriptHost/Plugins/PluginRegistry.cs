using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;

namespace Sunrise.ScriptHost.Plugins;

public sealed record PluginCommandContext(
    string Capability,
    JsonElement Payload,
    IReadOnlyDictionary<string, string> Variables);

public sealed record PluginCommandResult(bool Success, string? Reason, JsonElement? Result = null);

public interface IHostPlugin
{
    string Id { get; }
    IReadOnlyCollection<string> Capabilities { get; }
    Task<PluginCommandResult> ExecuteAsync(PluginCommandContext context, CancellationToken cancellationToken);
}

public sealed class PluginRegistry
{
    private readonly Dictionary<string, IHostPlugin> _byCapability;

    private PluginRegistry(Dictionary<string, IHostPlugin> byCapability)
    {
        _byCapability = byCapability;
    }

    public IEnumerable<string> Capabilities => _byCapability.Keys;

    public static PluginRegistry Load(string directory)
    {
        var plugins = new Dictionary<string, IHostPlugin>(StringComparer.Ordinal);
        if (!Directory.Exists(directory))
        {
            return new PluginRegistry(plugins);
        }

        foreach (string assemblyPath in Directory.EnumerateFiles(directory, "*.dll", SearchOption.TopDirectoryOnly))
        {
            try
            {
                Assembly assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
                foreach (Type type in assembly.GetTypes())
                {
                    if (type.IsAbstract || !typeof(IHostPlugin).IsAssignableFrom(type))
                    {
                        continue;
                    }

                    if (Activator.CreateInstance(type) is not IHostPlugin plugin)
                    {
                        continue;
                    }

                    foreach (string capability in plugin.Capabilities)
                    {
                        if (!plugins.TryAdd(capability, plugin))
                        {
                            throw new InvalidOperationException(
                                $"Capability '{capability}' is provided by more than one plugin.");
                        }
                    }

                    Console.WriteLine($"Loaded plugin '{plugin.Id}' from {assemblyPath}.");
                }
            }
            catch (Exception exception) when (
                exception is BadImageFormatException
                or FileLoadException
                or ReflectionTypeLoadException
                or InvalidOperationException)
            {
                Console.Error.WriteLine($"Plugin load failed for '{assemblyPath}': {exception.Message}");
            }
        }

        return new PluginRegistry(plugins);
    }

    public bool TryGet(string capability, out IHostPlugin? plugin) =>
        _byCapability.TryGetValue(capability, out plugin);
}
