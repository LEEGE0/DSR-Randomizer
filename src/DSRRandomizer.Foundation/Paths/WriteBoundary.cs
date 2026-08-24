namespace DSRRandomizer.Foundation.Paths;

public sealed class WriteBoundary
{
    private readonly string _sourceRoot;
    private readonly string _localRoot;
    private readonly IPathCanonicalizer _canonicalizer;

    private WriteBoundary(
        string sourceRoot,
        string localRoot,
        IPathCanonicalizer canonicalizer)
    {
        _sourceRoot = sourceRoot;
        _localRoot = localRoot;
        _canonicalizer = canonicalizer;
    }

    public static WriteBoundary Create(
        string sourceInstallation,
        string localDataRoot,
        IPathCanonicalizer canonicalizer)
    {
        ArgumentNullException.ThrowIfNull(canonicalizer);

        var sourceRoot = canonicalizer.Canonicalize(sourceInstallation);
        var localRoot = canonicalizer.Canonicalize(localDataRoot);
        if (IsAtOrBelow(sourceRoot, localRoot) || IsAtOrBelow(localRoot, sourceRoot))
        {
            throw new ArgumentException("Source and local-data roots must not overlap.");
        }

        return new WriteBoundary(sourceRoot, localRoot, canonicalizer);
    }

    public void EnsureAllowed(string candidatePath)
    {
        var candidate = _canonicalizer.Canonicalize(candidatePath);
        if (IsAtOrBelow(candidate, _sourceRoot) || !IsAtOrBelow(candidate, _localRoot))
        {
            throw new UnauthorizedAccessException(
                $"Write denied outside the randomizer data root: {candidate}");
        }
    }

    private static bool IsAtOrBelow(string candidate, string root)
    {
        var normalizedRoot = root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return candidate.Equals(normalizedRoot, StringComparison.OrdinalIgnoreCase)
            || candidate.StartsWith(
                normalizedRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase);
    }
}
