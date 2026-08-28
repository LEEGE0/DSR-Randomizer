namespace DSRRandomizer.Foundation.Packaging;

public sealed class ReleaseContentGuard
{
    private static readonly string[] RequiredPaths =
    [
        "DSRRandomizer.Launcher.exe",
        "README.md",
        "LICENSE",
        "CHANGELOG.md",
        "THIRD_PARTY_NOTICES.md",
        "config/compatibility-profiles.json",
        "native/DSRRandomizer.Runtime.dll",
        "native/DSRRandomizer.Runtime.dll.sha256"
    ];

    private static readonly HashSet<string> AllowedPaths = new(
        new[]
        {
            "DSRRandomizer.Launcher.exe",
            "DSRRandomizer.Launcher.pdb",
            "README.md",
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "CHANGELOG.md",
            "native/DSRRandomizer.Runtime.dll",
            "native/DSRRandomizer.Runtime.dll.sha256",
            "config/compatibility-profiles.json"
        },
        StringComparer.OrdinalIgnoreCase);

    public IReadOnlyList<string> Validate(IEnumerable<string> relativePaths)
    {
        ArgumentNullException.ThrowIfNull(relativePaths);
        var paths = relativePaths.ToArray();
        var duplicatePaths = paths
            .GroupBy(Normalize, StringComparer.OrdinalIgnoreCase)
            .Where(group => group.Count() > 1)
            .SelectMany(group => group)
            .ToHashSet(StringComparer.Ordinal);
        var prohibited = new List<string>();

        foreach (var originalPath in paths)
        {
            var normalized = Normalize(originalPath);
            if (duplicatePaths.Contains(originalPath)
                || string.IsNullOrWhiteSpace(normalized)
                || Path.IsPathRooted(originalPath)
                || normalized is "." or ".."
                || !AllowedPaths.Contains(normalized))
            {
                prohibited.Add(originalPath);
            }
        }

        var present = paths
            .Select(Normalize)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var requiredPath in RequiredPaths)
        {
            if (!present.Contains(requiredPath))
            {
                prohibited.Add($"missing:{requiredPath}");
            }
        }

        return prohibited;
    }

    private static string Normalize(string path) =>
        path?.Replace('\\', '/').Trim() ?? string.Empty;
}
