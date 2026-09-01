using System.Text;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.RmmBridgeHost;

internal static class BridgeHostFailureLog
{
    public static void WriteGameParamEvent(string externalRoot, string message)
    {
        try
        {
            WriteStrictGameParamEntry(externalRoot, message);
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

            WriteStrictGameParamEntry(externalRoot, exception.ToString());
        }
        catch
        {
            // Failure reporting must never replace the original fail-closed exit.
        }
    }

    private static void WriteStrictGameParamEntry(string externalRoot, string message)
    {
        if (!Path.IsPathFullyQualified(externalRoot)
            || externalRoot.StartsWith(@"\\", StringComparison.Ordinal))
        {
            throw new UnauthorizedAccessException("The GameParam log root must be an absolute local path.");
        }

        string fullExternal = Path.TrimEndingDirectorySeparator(Path.GetFullPath(externalRoot));
        if (!Directory.Exists(fullExternal))
            throw new DirectoryNotFoundException("The GameParam log root is missing.");

        var canonicalizer = new WindowsPathCanonicalizer();
        RejectReparse(fullExternal);
        string canonicalExternal = canonicalizer.Canonicalize(fullExternal);
        if (!canonicalExternal.Equals(fullExternal, StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("The GameParam log root is not canonical.");

        string logs = Path.Combine(fullExternal, "logs");
        EnsureNoReparseSegments(fullExternal, logs);
        Directory.CreateDirectory(logs);
        EnsureNoReparseSegments(fullExternal, logs);
        string canonicalLogs = canonicalizer.Canonicalize(logs);
        RequireContained(canonicalLogs, canonicalExternal);
        if (!canonicalLogs.Equals(logs, StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("The GameParam logs directory is not canonical.");

        string path = Path.Combine(logs, "rmm-bridge-host.log");
        EnsureNoReparseSegments(fullExternal, path);
        string canonicalPath = canonicalizer.Canonicalize(path);
        RequireContained(canonicalPath, canonicalExternal);
        if (!canonicalPath.Equals(path, StringComparison.OrdinalIgnoreCase))
            throw new UnauthorizedAccessException("The GameParam log file is not canonical.");

        var entry = $"{DateTimeOffset.UtcNow:O} {message}{Environment.NewLine}";
        File.AppendAllText(path, entry, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static void EnsureNoReparseSegments(string root, string candidate)
    {
        RequireContained(candidate, root);
        string current = root;
        RejectReparse(current);
        foreach (string segment in Path.GetRelativePath(root, candidate).Split(
                     [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (!Directory.Exists(current) && !File.Exists(current))
                break;
            RejectReparse(current);
        }
    }

    private static void RejectReparse(string path)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new UnauthorizedAccessException($"Reparse points are not allowed in the GameParam log path: {path}");
    }

    private static void RequireContained(string candidate, string root)
    {
        string normalizedRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string normalizedCandidate = Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
        if (!normalizedCandidate.Equals(normalizedRoot, StringComparison.OrdinalIgnoreCase)
            && !normalizedCandidate.StartsWith(
                normalizedRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new UnauthorizedAccessException("The GameParam log path escapes the external root.");
        }
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
