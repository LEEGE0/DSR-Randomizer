namespace DSRRandomizer.Foundation.Runtime;

internal static class RuntimePathSafety
{
    public static string ResolveUnderRoot(string root, string relativePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        ArgumentException.ThrowIfNullOrWhiteSpace(relativePath);
        if (Path.IsPathRooted(relativePath))
        {
            throw new UnauthorizedAccessException($"Rooted catalog path is not allowed: {relativePath}");
        }

        var normalizedRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        var platformPath = relativePath.Replace('/', Path.DirectorySeparatorChar);
        var candidate = Path.GetFullPath(Path.Combine(normalizedRoot, platformPath));
        if (!candidate.StartsWith(
                normalizedRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new UnauthorizedAccessException($"Path escapes its declared root: {relativePath}");
        }

        return candidate;
    }
}
