using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Installation;

public sealed class InstallationSelectionStore
{
    private const string FileName = "source-installation.json";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private readonly string _configPath;
    private readonly WriteBoundary? _boundary;
    private readonly IPathCanonicalizer _canonicalizer;

    public InstallationSelectionStore(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IPathCanonicalizer canonicalizer)
    {
        _configPath = layout.Config;
        _boundary = boundary;
        _canonicalizer = canonicalizer;
    }

    private InstallationSelectionStore(
        string configPath,
        IPathCanonicalizer canonicalizer)
    {
        _configPath = configPath;
        _canonicalizer = canonicalizer;
    }

    public static InstallationSelectionStore CreateReadOnly(
        string localDataRoot,
        IPathCanonicalizer canonicalizer) =>
        new(Path.Combine(Path.GetFullPath(localDataRoot), "config"), canonicalizer);

    public async Task<string?> ReadAsync(CancellationToken cancellationToken)
    {
        var path = Path.Combine(_configPath, FileName);
        if (!File.Exists(path))
        {
            return null;
        }

        await using var stream = File.OpenRead(path);
        var selection = await JsonSerializer.DeserializeAsync<InstallationSelection>(
                stream,
                JsonOptions,
                cancellationToken)
            .ConfigureAwait(false);
        return selection is null
            ? null
            : _canonicalizer.Canonicalize(selection.CanonicalInstallationPath);
    }

    public async Task SaveAsync(
        string installationPath,
        CancellationToken cancellationToken)
    {
        var canonicalPath = _canonicalizer.Canonicalize(installationPath);
        var boundary = _boundary
            ?? throw new InvalidOperationException("This selection store is read-only.");
        var path = Path.Combine(_configPath, FileName);
        var temporaryPath = Path.Combine(
            _configPath,
            $"source-installation.{Guid.NewGuid():N}.tmp");
        boundary.EnsureAllowed(_configPath);
        boundary.EnsureAllowed(path);
        boundary.EnsureAllowed(temporaryPath);
        Directory.CreateDirectory(_configPath);

        try
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(
                new InstallationSelection(canonicalPath),
                JsonOptions);
            await File.WriteAllBytesAsync(temporaryPath, bytes, cancellationToken);
            File.Move(temporaryPath, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }

    private sealed record InstallationSelection(string CanonicalInstallationPath);
}
