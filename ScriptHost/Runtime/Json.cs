using System.Text.Json;
using System.Text.Json.Serialization;

namespace Sunrise.ScriptHost.Runtime;

public static class Json
{
    public static JsonSerializerOptions DefaultOptions { get; } = CreateOptions(writeIndented: true);
    public static JsonSerializerOptions ProtocolOptions { get; } = CreateOptions(writeIndented: false);

    private static JsonSerializerOptions CreateOptions(bool writeIndented)
    {
        var options = new JsonSerializerOptions
        {
            AllowTrailingCommas = false,
            PropertyNameCaseInsensitive = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            ReadCommentHandling = JsonCommentHandling.Skip,
            WriteIndented = writeIndented,
        };
        options.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return options;
    }
}

public static class FileName
{
    public static string Safe(string value)
    {
        char[] invalid = Path.GetInvalidFileNameChars();
        var result = new char[value.Length];
        for (int index = 0; index < value.Length; ++index)
        {
            char character = value[index];
            result[index] = invalid.Contains(character) ? '_' : character;
        }

        return new string(result);
    }
}
