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

    public SaveSelectionStore(
        LocalDataLayout layout,
        WriteBoundary boundary)
    {
        ArgumentNullException.ThrowIfNull(layout);
        ArgumentNullException.ThrowIfNull(boundary);

        _configPath = layout.Config;
        _boundary = boundary;
    }

    public async Task<SaveProfileCandidate?> ReadAsync(CancellationToken cancellationToken)
    {
        var path = Path.Combine(_configPath, FileName);
        _boundary.EnsureAllowed(_configPath);
        _boundary.EnsureAllowed(path);
        if (!File.Exists(path))
        {
            return null;
        }

        await using var stream = File.OpenRead(path);
        var selection = await JsonSerializer.DeserializeAsync<SaveProfileCandidate>(
            stream,
            JsonOptions,
            cancellationToken);
        return selection is null ? null : NormalizeAndValidate(selection);
    }

    public async Task WriteAsync(
        SaveProfileCandidate selection,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(selection);

        var canonicalSelection = NormalizeAndValidate(selection);
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

    private static SaveProfileCandidate NormalizeAndValidate(SaveProfileCandidate selection)
    {
        if (string.IsNullOrWhiteSpace(selection.SteamId) || !SteamIdPattern().IsMatch(selection.SteamId))
        {
            throw new ArgumentException("Steam ID must contain 1 to 20 decimal digits.", nameof(selection));
        }

        if (string.IsNullOrWhiteSpace(selection.SourcePath))
        {
            throw new ArgumentException("Selection must include a source path.", nameof(selection));
        }

        var canonicalSourcePath = Path.GetFullPath(selection.SourcePath);
        var parentDirectory = Path.GetDirectoryName(canonicalSourcePath);
        if (!Path.GetFileName(canonicalSourcePath).Equals(NormalSaveName, StringComparison.OrdinalIgnoreCase)
            || parentDirectory is null
            || !Path.GetFileName(parentDirectory).Equals(selection.SteamId, StringComparison.Ordinal))
        {
            throw new ArgumentException("Selection must reference an exact normal save path.", nameof(selection));
        }

        return new SaveProfileCandidate(selection.SteamId, canonicalSourcePath);
    }

    [GeneratedRegex("^[0-9]{1,20}$", RegexOptions.CultureInvariant)]
    private static partial Regex SteamIdPattern();
}
