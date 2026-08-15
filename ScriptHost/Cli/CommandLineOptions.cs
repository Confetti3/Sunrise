namespace Sunrise.ScriptHost.Cli;

public sealed record CommandLineOptions
{
    public required string ScenarioPath { get; init; }
    public required string BindingPath { get; init; }
    public required string ScriptRoot { get; init; }
    public required string PluginRoot { get; init; }
    public required string StateRoot { get; init; }
    public string? GameRoot { get; init; }
    public string? SunriseRoot { get; init; }
    public string PipeName { get; init; } = "sunrise-script-host-v1";
    public bool HashBinaries { get; init; }
    public bool ValidateOnly { get; init; }
    public bool SelfTest { get; init; }
    public bool ShowHelp { get; init; }

    public static CommandLineOptions Parse(IReadOnlyList<string> args)
    {
        string baseDirectory = AppContext.BaseDirectory;
        string scriptRoot = Path.Combine(baseDirectory, "scripts");
        string pluginRoot = Path.Combine(baseDirectory, "plugins");
        string stateRoot = Path.Combine(baseDirectory, "state");
        string scenarioPath = Path.Combine(scriptRoot, "red-war-first-mission.prototype.json");
        string bindingPath = Path.Combine(baseDirectory, "bindings", "build-86657.json");
        string pipeName = "sunrise-script-host-v1";
        string? gameRoot = null;
        string? sunriseRoot = null;
        bool hashBinaries = false;
        bool validateOnly = false;
        bool selfTest = false;
        bool showHelp = false;

        for (int index = 0; index < args.Count; ++index)
        {
            string argument = args[index];
            switch (argument)
            {
                case "--scenario":
                    scenarioPath = RequiredValue(args, ref index, argument);
                    break;
                case "--binding":
                    bindingPath = RequiredValue(args, ref index, argument);
                    break;
                case "--script-root":
                    scriptRoot = RequiredValue(args, ref index, argument);
                    break;
                case "--plugin-root":
                    pluginRoot = RequiredValue(args, ref index, argument);
                    break;
                case "--state-root":
                    stateRoot = RequiredValue(args, ref index, argument);
                    break;
                case "--game-root":
                    gameRoot = RequiredValue(args, ref index, argument);
                    break;
                case "--sunrise-root":
                    sunriseRoot = RequiredValue(args, ref index, argument);
                    break;
                case "--pipe":
                    pipeName = NormalizePipeName(RequiredValue(args, ref index, argument));
                    break;
                case "--hash-binaries":
                    hashBinaries = true;
                    break;
                case "--validate":
                    validateOnly = true;
                    break;
                case "--self-test":
                    selfTest = true;
                    break;
                case "--help":
                case "-h":
                    showHelp = true;
                    break;
                default:
                    throw new ArgumentException($"Unknown argument: {argument}");
            }
        }

        scriptRoot = FullPath(scriptRoot);
        pluginRoot = FullPath(pluginRoot);
        stateRoot = FullPath(stateRoot);
        scenarioPath = FullPath(scenarioPath);
        bindingPath = FullPath(bindingPath);
        gameRoot = gameRoot is null ? null : FullPath(gameRoot);
        sunriseRoot = sunriseRoot is null ? null : FullPath(sunriseRoot);

        if (!showHelp && !selfTest)
        {
            RequireFile(scenarioPath, "scenario");
            RequireFile(bindingPath, "binding manifest");
        }

        return new CommandLineOptions
        {
            ScenarioPath = scenarioPath,
            BindingPath = bindingPath,
            ScriptRoot = scriptRoot,
            PluginRoot = pluginRoot,
            StateRoot = stateRoot,
            GameRoot = gameRoot,
            SunriseRoot = sunriseRoot,
            PipeName = pipeName,
            HashBinaries = hashBinaries,
            ValidateOnly = validateOnly,
            SelfTest = selfTest,
            ShowHelp = showHelp,
        };
    }

    public static void WriteHelp(TextWriter writer)
    {
        writer.WriteLine("Sunrise.ScriptHost");
        writer.WriteLine();
        writer.WriteLine("  --scenario <file>       Scenario JSON to run.");
        writer.WriteLine("  --binding <file>        Build capability manifest.");
        writer.WriteLine("  --script-root <dir>     Explicit script search root.");
        writer.WriteLine("  --plugin-root <dir>     Managed plugin directory.");
        writer.WriteLine("  --state-root <dir>      Checkpoint and probe-report directory.");
        writer.WriteLine("  --game-root <dir>       Optional Destiny build root to inspect.");
        writer.WriteLine("  --sunrise-root <dir>    Optional installed Sunrise root to inspect.");
        writer.WriteLine("  --pipe <name>           Named pipe name or full \\.\\pipe\\ path.");
        writer.WriteLine("  --hash-binaries         SHA-256 the discovered EXE and proxy DLL.");
        writer.WriteLine("  --validate              Validate inputs without opening the pipe.");
        writer.WriteLine("  --self-test             Run the deterministic host self-test.");
        writer.WriteLine("  --help                  Show this help.");
    }

    private static string RequiredValue(IReadOnlyList<string> args, ref int index, string option)
    {
        if (index + 1 >= args.Count)
        {
            throw new ArgumentException($"{option} requires a value.");
        }

        string value = args[++index];
        if (string.IsNullOrWhiteSpace(value) || value.StartsWith("--", StringComparison.Ordinal))
        {
            throw new ArgumentException($"{option} requires a value.");
        }

        return value;
    }

    private static string NormalizePipeName(string value)
    {
        const string prefix = @"\\.\pipe\";
        string name = value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
            ? value[prefix.Length..]
            : value;
        if (string.IsNullOrWhiteSpace(name) || name.Contains('\\') || name.Contains('/'))
        {
            throw new ArgumentException("The pipe name must be one Windows named-pipe leaf name.");
        }

        return name;
    }

    private static string FullPath(string value) => Path.GetFullPath(
        Environment.ExpandEnvironmentVariables(value));

    private static void RequireFile(string path, string description)
    {
        if (!File.Exists(path))
        {
            throw new ArgumentException($"The {description} does not exist: {path}");
        }
    }
}
