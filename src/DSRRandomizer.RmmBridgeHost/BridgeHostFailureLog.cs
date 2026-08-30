using System.Text;

namespace DSRRandomizer.RmmBridgeHost;

internal static class BridgeHostFailureLog
{
    public static void WriteGameParamEvent(string externalRoot, string message)
    {
        try
        {
            WriteEntry(externalRoot, message);
        }
        catch
        {
            // Publication diagnostics must never replace the fail-closed result.
        }
    }

    public static void Write(IReadOnlyList<string> arguments, Exception exception)
    {
        try
        {
            var externalRoot = FindExternalRoot(arguments);
            if (externalRoot is null)
            {
                return;
            }

            WriteEntry(externalRoot, exception.ToString());
        }
        catch
        {
            // Failure reporting must never replace the original fail-closed exit.
        }
    }

    private static void WriteEntry(string externalRoot, string message)
    {
        var logs = Path.Combine(externalRoot, "logs");
        Directory.CreateDirectory(logs);
        var path = Path.Combine(logs, "rmm-bridge-host.log");
        var entry = $"{DateTimeOffset.UtcNow:O} {message}{Environment.NewLine}";
        File.AppendAllText(path, entry, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static string? FindExternalRoot(IReadOnlyList<string> arguments)
    {
        for (var index = 0; index + 1 < arguments.Count; index += 2)
        {
            if (!arguments[index].Equals("--external-root", StringComparison.Ordinal))
            {
                continue;
            }

            var candidate = arguments[index + 1];
            if (!Path.IsPathFullyQualified(candidate)
                || candidate.StartsWith(@"\\", StringComparison.Ordinal)
                || candidate.Contains("..", StringComparison.Ordinal))
            {
                return null;
            }
            return Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
        }
        return null;
    }
}
