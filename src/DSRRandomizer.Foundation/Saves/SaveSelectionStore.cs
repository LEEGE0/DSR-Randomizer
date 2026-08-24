using System.Text.Json;
using System.Text.RegularExpressions;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Saves;

public sealed partial class SaveSelectionStore
{
    private const string FileName = "selected-save-profile.json";
    private const string NormalSaveName = "DRAKS0005.sl2";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private readonly string _configPath;
    private readonly WriteBoundary _boundary;
    private readonly IPathCanonicalizer _canonicalizer;

    public SaveSelectionStore(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IPathCanonicalizer canonicalizer)
    {
        ArgumentNullException.ThrowIfNull(layout);
        ArgumentNullException.ThrowIfNull(boundary);
        ArgumentNullException.ThrowIfNull(canonicalizer);

        _configPath = layout.Config;
        _boundary = boundary;
        _canonicalizer = canonicalizer;
    }

    public async Task<SaveProfileCandidate?> ReadAsync(CancellationToken cancellationToken)
    {
        var path = Path.Combine(_configPath, FileName);
        if (!File.Exists(path))
        {
            return null;
        }

        await using var stream = File.OpenRead(path);
        var selection = await JsonSerializer.DeserializeAsync<SaveProfileCandidate>(
            stream,
            JsonOptions,
            cancellationToken);
        return selection is null ? null : CanonicalizeAndValidate(selection);
    }

    public async Task WriteAsync(
        SaveProfileCandidate selection,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(selection);

        var canonicalSelection = CanonicalizeAndValidate(selection);
        var path = Path.Combine(_configPath, FileName);
        var temporaryPath = Path.Combine(
            _configPath,
            $"selected-save-profile.{Guid.NewGuid():N}.tmp");
        _boundary.EnsureAllowed(_configPath);
        _boundary.EnsureAllowed(path);
        _boundary.EnsureAllowed(temporaryPath);
        Directory.CreateDirectory(_configPath);

        try
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(canonicalSelection, JsonOptions);
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

    private SaveProfileCandidate CanonicalizeAndValidate(SaveProfileCandidate selection)
    {
        if (string.IsNullOrWhiteSpace(selection.SteamId) || !SteamIdPattern().IsMatch(selection.SteamId))
        {
            throw new ArgumentException("Steam ID must contain 16 to 20 decimal digits.", nameof(selection));
        }

        var canonicalSourcePath = _canonicalizer.Canonicalize(selection.SourcePath);
        var parentDirectory = Path.GetDirectoryName(canonicalSourcePath);
        if (!Path.GetFileName(canonicalSourcePath).Equals(NormalSaveName, StringComparison.OrdinalIgnoreCase)
            || parentDirectory is null
            || !Path.GetFileName(parentDirectory).Equals(selection.SteamId, StringComparison.Ordinal))
        {
            throw new ArgumentException("Selection must reference an exact normal save path.", nameof(selection));
        }

        return new SaveProfileCandidate(selection.SteamId, canonicalSourcePath);
    }

    [GeneratedRegex("^[0-9]{16,20}$", RegexOptions.CultureInvariant)]
    private static partial Regex SteamIdPattern();
}
