using System.Globalization;
using System.Text.RegularExpressions;

namespace DSRRandomizer.RmmBridgeHost;

public sealed record BridgeHostArguments(
    uint GamePid,
    string ExternalRoot,
    string RuntimeId,
    string SteamId,
    string ReadyEventName)
{
    private static readonly Regex RuntimePattern = new(
        "^runtime-[0-9a-f]{8,128}$",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);
    private static readonly Regex SteamIdPattern = new(
        "^[0-9]{1,20}$",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);
    private static readonly Regex ReadyEventPattern = new(
        @"^Local\\DSRRandomizer\.RmmBridge\.[0-9a-f]{32}$",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking);

    public static BridgeHostArguments Parse(IReadOnlyList<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        if (arguments.Count != 10)
        {
            throw new ArgumentException("The bridge host requires exactly five switch/value pairs.");
        }

        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var index = 0; index < arguments.Count; index += 2)
        {
            var name = arguments[index];
            var value = arguments[index + 1];
            if (name is not ("--game-pid" or "--external-root" or "--runtime-id"
                or "--steam-id" or "--ready-event")
                || !values.TryAdd(name, value))
            {
                throw new ArgumentException($"Unknown or duplicate bridge-host switch: {name}");
            }
        }

        if (!uint.TryParse(values["--game-pid"], NumberStyles.None,
                CultureInfo.InvariantCulture, out var gamePid)
            || gamePid == 0)
        {
            throw new ArgumentException("The game PID must be a nonzero unsigned integer.");
        }

        var externalRoot = values["--external-root"];
        if (!Path.IsPathFullyQualified(externalRoot)
            || externalRoot.StartsWith(@"\\", StringComparison.Ordinal)
            || externalRoot.Contains("..", StringComparison.Ordinal))
        {
            throw new ArgumentException("The external root must be an absolute local path.");
        }

        var runtimeId = values["--runtime-id"];
        var steamId = values["--steam-id"];
        var readyEvent = values["--ready-event"];
        if (!RuntimePattern.IsMatch(runtimeId))
        {
            throw new ArgumentException("The runtime ID is invalid.");
        }
        if (!SteamIdPattern.IsMatch(steamId))
        {
            throw new ArgumentException("The Steam ID must contain 1 to 20 decimal digits.");
        }
        if (!ReadyEventPattern.IsMatch(readyEvent))
        {
            throw new ArgumentException("The ready-event name is invalid.");
        }

        return new BridgeHostArguments(
            gamePid,
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(externalRoot)),
            runtimeId,
            steamId,
            readyEvent);
    }
}
