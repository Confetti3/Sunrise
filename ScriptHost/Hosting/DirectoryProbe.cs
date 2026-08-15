using System.Security.Cryptography;
using System.Text.Json;
using Sunrise.ScriptHost.Cli;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost.Hosting;

public sealed record ProbeItem(string Kind, string Path, bool Exists, long? Length, string? Sha256);

public sealed record ProbeReport(
    DateTimeOffset ObservedAtUtc,
    string? GameRoot,
    string? SunriseRoot,
    string ScriptRoot,
    string PluginRoot,
    string StateRoot,
    IReadOnlyList<ProbeItem> Items,
    int PackageFileCount,
    IReadOnlyList<string> Warnings);

public static class DirectoryProbe
{
    public static ProbeReport Run(CommandLineOptions options)
    {
        var items = new List<ProbeItem>();
        var warnings = new List<string>();

        AddFile(items, "scenario", options.ScenarioPath, hash: false);
        AddFile(items, "binding", options.BindingPath, hash: false);
        AddDirectory(items, "script-root", options.ScriptRoot);
        AddDirectory(items, "plugin-root", options.PluginRoot);
        AddDirectory(items, "state-root", options.StateRoot);

        if (options.GameRoot is not null)
        {
            AddDirectory(items, "game-root", options.GameRoot);
            AddFile(items, "destiny-executable", Path.Combine(options.GameRoot, "destiny2.exe"), options.HashBinaries);
        }

        if (options.SunriseRoot is not null)
        {
            AddDirectory(items, "sunrise-root", options.SunriseRoot);
            AddFile(items, "sunrise-proxy", Path.Combine(options.SunriseRoot, "steam_api64.dll"), options.HashBinaries);
            AddFile(
                items,
                "sunrise-log",
                Path.Combine(options.SunriseRoot, "Sunrise", "logs", "sunrise.log"),
                hash: false);
        }

        int packageFileCount = CountPackages(options.GameRoot, warnings);
        if (options.GameRoot is null)
        {
            warnings.Add("No --game-root was supplied; build and package probes are intentionally limited.");
        }

        if (options.SunriseRoot is null)
        {
            warnings.Add("No --sunrise-root was supplied; the installed proxy DLL and Sunrise log were not inspected.");
        }

        return new ProbeReport(
            DateTimeOffset.UtcNow,
            options.GameRoot,
            options.SunriseRoot,
            options.ScriptRoot,
            options.PluginRoot,
            options.StateRoot,
            items,
            packageFileCount,
            warnings);
    }

    public static async Task SaveAsync(string path, ProbeReport report, CancellationToken cancellationToken)
    {
        string? directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        await using FileStream stream = File.Create(path);
        await JsonSerializer.SerializeAsync(
            stream,
            report,
            Json.DefaultOptions,
            cancellationToken).ConfigureAwait(false);
    }

    public static void PrintSummary(ProbeReport report, string reportPath)
    {
        int present = report.Items.Count(item => item.Exists);
        Console.WriteLine($"Probe: {present}/{report.Items.Count} requested paths present; {report.PackageFileCount} .pkg files observed.");
        foreach (string warning in report.Warnings)
        {
            Console.WriteLine($"Probe warning: {warning}");
        }

        Console.WriteLine($"Probe report: {reportPath}");
    }

    private static void AddDirectory(ICollection<ProbeItem> items, string kind, string path)
    {
        var info = new DirectoryInfo(path);
        items.Add(new ProbeItem(kind, info.FullName, info.Exists, null, null));
    }

    private static void AddFile(ICollection<ProbeItem> items, string kind, string path, bool hash)
    {
        var info = new FileInfo(path);
        string? sha256 = null;
        if (info.Exists && hash)
        {
            using FileStream stream = info.OpenRead();
            sha256 = Convert.ToHexString(SHA256.HashData(stream));
        }

        items.Add(new ProbeItem(kind, info.FullName, info.Exists, info.Exists ? info.Length : null, sha256));
    }

    private static int CountPackages(string? gameRoot, ICollection<string> warnings)
    {
        if (gameRoot is null || !Directory.Exists(gameRoot))
        {
            return 0;
        }

        try
        {
            const int maximumRecorded = 100_000;
            int count = Directory.EnumerateFiles(gameRoot, "*.pkg", SearchOption.AllDirectories)
                .Take(maximumRecorded + 1)
                .Count();
            if (count > maximumRecorded)
            {
                warnings.Add($"Package count exceeded the defensive cap of {maximumRecorded:N0}.");
                return maximumRecorded;
            }

            return count;
        }
        catch (UnauthorizedAccessException exception)
        {
            warnings.Add($"Package scan was incomplete: {exception.Message}");
            return 0;
        }
        catch (IOException exception)
        {
            warnings.Add($"Package scan was incomplete: {exception.Message}");
            return 0;
        }
    }
}
