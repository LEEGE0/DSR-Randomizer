using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Installation;

public sealed class GameInstallationVerifier
{
    private readonly IPathCanonicalizer _canonicalizer;
    private readonly string _localDataRoot;

    public GameInstallationVerifier(IPathCanonicalizer canonicalizer, string localDataRoot)
    {
        ArgumentNullException.ThrowIfNull(canonicalizer);
        _canonicalizer = canonicalizer;
        _localDataRoot = canonicalizer.Canonicalize(localDataRoot);
    }

    public async Task<VerificationResult> VerifyAsync(
        string installationPath,
        CancellationToken cancellationToken)
    {
        await Task.Yield();
        cancellationToken.ThrowIfCancellationRequested();

        string canonicalInstallationPath;
        try
        {
            canonicalInstallationPath = _canonicalizer.Canonicalize(installationPath);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return Invalid(string.Empty, $"Installation path could not be resolved: {exception.Message}");
        }

        if (!Directory.Exists(canonicalInstallationPath))
        {
            return Invalid(canonicalInstallationPath, "The selected installation path does not exist.");
        }

        if (IsAtOrBelow(canonicalInstallationPath, _localDataRoot))
        {
            return Invalid(
                canonicalInstallationPath,
                "The game installation cannot be inside the randomizer local-data root.");
        }

        var errors = ValidateRequiredLayout(canonicalInstallationPath);
        if (errors.Count > 0)
        {
            return new VerificationResult(false, canonicalInstallationPath, null, errors);
        }

        try
        {
            var catalog = BuildCatalog(canonicalInstallationPath, cancellationToken);
            return new VerificationResult(
                true,
                canonicalInstallationPath,
                catalog,
                Array.Empty<string>());
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return Invalid(canonicalInstallationPath, exception.Message);
        }
    }

    private static List<string> ValidateRequiredLayout(string root)
    {
        var errors = new List<string>();
        if (!File.Exists(Path.Combine(root, "DarkSoulsRemastered.exe")))
        {
            errors.Add("Required file is missing: DarkSoulsRemastered.exe");
        }

        foreach (var directory in StockGameLayout.DataDirectories.Order(StringComparer.Ordinal))
        {
            if (!Directory.Exists(Path.Combine(root, directory)))
            {
                errors.Add($"Required data directory is missing: {directory}");
            }
        }

        return errors;
    }

    private static GameFileCatalog BuildCatalog(string root, CancellationToken cancellationToken)
    {
        var entries = new Dictionary<string, GameFileEntry>(StringComparer.OrdinalIgnoreCase);

        foreach (var rootFile in StockGameLayout.RootFiles.Order(StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var path = Path.Combine(root, rootFile);
            if (!File.Exists(path))
            {
                continue;
            }

            AddFile(root, path, entries);
        }

        foreach (var dataDirectory in StockGameLayout.DataDirectories.Order(StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var path = Path.Combine(root, dataDirectory);
            RejectReparsePoint(path);
            AddDirectoryFiles(root, path, entries, cancellationToken);
        }

        var files = entries.Values
            .OrderBy(entry => entry.RelativePath, StringComparer.Ordinal)
            .ToArray();
        var totalBytes = files.Aggregate(0L, (total, entry) => checked(total + entry.Length));
        return new GameFileCatalog(files, totalBytes);
    }

    private static void AddDirectoryFiles(
        string root,
        string initialDirectory,
        Dictionary<string, GameFileEntry> entries,
        CancellationToken cancellationToken)
    {
        var pending = new Stack<string>();
        pending.Push(initialDirectory);

        while (pending.TryPop(out var directory))
        {
            cancellationToken.ThrowIfCancellationRequested();
            foreach (var item in new DirectoryInfo(directory).EnumerateFileSystemInfos())
            {
                cancellationToken.ThrowIfCancellationRequested();
                RejectReparsePoint(item.FullName);
                if ((item.Attributes & FileAttributes.Directory) != 0)
                {
                    pending.Push(item.FullName);
                }
                else
                {
                    AddFile(root, item.FullName, entries);
                }
            }
        }
    }

    private static void AddFile(
        string root,
        string path,
        Dictionary<string, GameFileEntry> entries)
    {
        RejectReparsePoint(path);
        var info = new FileInfo(path);
        var relativePath = Path.GetRelativePath(root, path)
            .Replace(Path.DirectorySeparatorChar, '/');
        var entry = new GameFileEntry(relativePath, info.Length, info.LastWriteTimeUtc);
        if (!entries.TryAdd(relativePath, entry))
        {
            throw new IOException($"Duplicate normalized game path: {relativePath}");
        }
    }

    private static void RejectReparsePoint(string path)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
        {
            throw new IOException($"Reparse points are not allowed in the source catalog: {path}");
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

    private static VerificationResult Invalid(string canonicalPath, string error) =>
        new(false, canonicalPath, null, new[] { error });
}
