using Sunrise.ScriptHost.Cli;
using Sunrise.ScriptHost.Hosting;
using Sunrise.ScriptHost.Plugins;
using Sunrise.ScriptHost.Protocol;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost;

internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        CommandLineOptions options;
        try
        {
            options = CommandLineOptions.Parse(args);
        }
        catch (ArgumentException exception)
        {
            Console.Error.WriteLine(exception.Message);
            Console.Error.WriteLine();
            CommandLineOptions.WriteHelp(Console.Error);
            return 2;
        }

        if (options.ShowHelp)
        {
            CommandLineOptions.WriteHelp(Console.Out);
            return 0;
        }

        try
        {
            Directory.CreateDirectory(options.StateRoot);

            var probe = DirectoryProbe.Run(options);
            string probePath = Path.Combine(options.StateRoot, "probe-report.json");
            await DirectoryProbe.SaveAsync(probePath, probe, CancellationToken.None).ConfigureAwait(false);
            DirectoryProbe.PrintSummary(probe, probePath);

            CapabilityCatalog capabilities = CapabilityCatalog.Load(options.BindingPath);
            ScenarioDefinition scenario = ScenarioLoader.Load(options.ScenarioPath);

            if (options.ValidateOnly)
            {
                Console.WriteLine($"Validated scenario '{scenario.Id}' and capability manifest '{capabilities.Build}'.");
                return 0;
            }

            if (options.SelfTest)
            {
                await SelfTest.RunAsync().ConfigureAwait(false);
                Console.WriteLine("Self-test passed.");
                return 0;
            }

            PluginRegistry plugins = PluginRegistry.Load(options.PluginRoot);
            capabilities.ReplacePluginCapabilities(plugins.Capabilities);

            await using var bridge = new NamedPipeBridge(options.PipeName);
            var checkpointStore = new CheckpointStore(
                Path.Combine(options.StateRoot, $"{FileName.Safe(scenario.Id)}.checkpoint.json"));
            var dispatcher = new CompositeCommandDispatcher(bridge, plugins);
            var runtime = new MissionRuntime(scenario, capabilities, dispatcher, checkpointStore);
            var service = new ScriptHostService(bridge, capabilities, runtime, dispatcher);

            using var cancellation = new CancellationTokenSource();
            Console.CancelKeyPress += (_, eventArgs) =>
            {
                eventArgs.Cancel = true;
                cancellation.Cancel();
            };

            Console.WriteLine($"Sunrise script host listening on \\.\\pipe\\{options.PipeName}");
            Console.WriteLine($"Scenario: {scenario.Id}");
            Console.WriteLine("Press Ctrl+C to stop.");

            await service.RunAsync(cancellation.Token).ConfigureAwait(false);
            return 0;
        }
        catch (OperationCanceledException)
        {
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
