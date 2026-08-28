using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Launcher.Configuration;

/// <summary>
/// Persists the one local bootstrap pointer to the user-selected external material root.
/// </summary>
public sealed class ExternalRootSelectionStore
{
    private const string PointerFileName = "external-root.json";
    private const int SchemaVersion = 1;

    private readonly string _localDataRoot;
    private readonly string? _sourceInstallationRoot;
    private readonly IPathCanonicalizer _canonicalizer;

    public ExternalRootSelectionStore(string localDataRoot, string? sourceInstallationRoot = null)
        : this(localDataRoot, sourceInstallationRoot, new WindowsPathCanonicalizer())
    {
    }

    internal ExternalRootSelectionStore(
        string localDataRoot,
        string? sourceInstallationRoot,
        IPathCanonicalizer canonicalizer)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(localDataRoot);
        ArgumentNullException.ThrowIfNull(canonicalizer);
        _localDataRoot = Path.GetFullPath(localDataRoot);
        _sourceInstallationRoot = string.IsNullOrWhiteSpace(sourceInstallationRoot)
            ? null
            : canonicalizer.Canonicalize(sourceInstallationRoot);
        _canonicalizer = canonicalizer;
    }

    public async Task<string?> ReadAsync(CancellationToken cancellationToken)
    {
        var path = PointerPath;
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            await using var stream = File.OpenRead(path);
            using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
            var root = ReadSchemaOneRoot(document.RootElement);
            return ValidateExternalRoot(root);
        }
        catch (JsonException exception)
        {
            throw new IOException("The external-root pointer is malformed.", exception);
        }
    }

    public Task WriteAsync(string externalRoot, CancellationToken cancellationToken) =>
        WriteAsync(externalRoot, sourceInstallationRoot: null, cancellationToken: cancellationToken);

    public async Task WriteAsync(
        string externalRoot,
        string? sourceInstallationRoot,
        CancellationToken cancellationToken)
    {
        var canonicalRoot = ValidateExternalRoot(externalRoot, sourceInstallationRoot);
        Directory.CreateDirectory(_localDataRoot);
        var temporaryPath = Path.Combine(
            _localDataRoot,
            $"external-root.{Guid.NewGuid():N}.tmp");
        try
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(
                new Pointer(SchemaVersion, canonicalRoot),
                new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
            await File.WriteAllBytesAsync(temporaryPath, bytes, cancellationToken);
            File.Move(temporaryPath, PointerPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }

    private string PointerPath => Path.Combine(_localDataRoot, PointerFileName);

    private static string ReadSchemaOneRoot(JsonElement element)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            throw new IOException("The external-root pointer must be a JSON object.");
        }

        var schemaVersionSeen = false;
        var rootSeen = false;
        var schemaVersion = 0;
        string? root = null;
        foreach (var property in element.EnumerateObject())
        {
            switch (property.Name)
            {
                case "schemaVersion" when !schemaVersionSeen
                    && property.Value.TryGetInt32(out var parsedSchemaVersion):
                    schemaVersion = parsedSchemaVersion;
                    schemaVersionSeen = true;
                    break;
                case "root" when !rootSeen && property.Value.ValueKind == JsonValueKind.String:
                    root = property.Value.GetString();
                    rootSeen = true;
                    break;
                default:
                    throw new IOException("The external-root pointer has unknown, duplicate, or invalid properties.");
            }
        }

        if (!schemaVersionSeen || !rootSeen || schemaVersion != SchemaVersion || string.IsNullOrWhiteSpace(root))
        {
            throw new IOException("The external-root pointer does not use the supported schema.");
        }

        return root;
    }

    private string ValidateExternalRoot(string suppliedRoot, string? sourceInstallationRoot = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(suppliedRoot);
        if (!Path.IsPathFullyQualified(suppliedRoot))
        {
            throw new ArgumentException("The external root must be an absolute path.", nameof(suppliedRoot));
        }

        var fullPath = Path.GetFullPath(suppliedRoot);
        if (!Directory.Exists(fullPath))
        {
            throw new DirectoryNotFoundException($"The external root does not exist: {fullPath}");
        }

        var root = Path.GetPathRoot(fullPath);
        if (root is not null && fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            .Equals(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar), StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The external root cannot be a filesystem root.", nameof(suppliedRoot));
        }

        if ((File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
        {
            throw new IOException($"The external root cannot be a reparse point: {fullPath}");
        }

        var canonicalRoot = _canonicalizer.Canonicalize(fullPath);
        EnsureNotSourceInstallationDescendant(canonicalRoot);
        var protectedSource = string.IsNullOrWhiteSpace(sourceInstallationRoot)
            ? _sourceInstallationRoot
            : _canonicalizer.Canonicalize(sourceInstallationRoot);
        if (protectedSource is not null && IsAtOrBelow(canonicalRoot, protectedSource))
        {
            throw new UnauthorizedAccessException(
                "The external root cannot be the source installation or one of its descendants.");
        }

        return canonicalRoot;
    }

    private static void EnsureNotSourceInstallationDescendant(string externalRoot)
    {
        var current = Path.TrimEndingDirectorySeparator(externalRoot);
        while (true)
        {
            if (File.Exists(Path.Combine(current, "DarkSoulsRemastered.exe")))
            {
                throw new UnauthorizedAccessException(
                    "The external root cannot be the source installation or one of its descendants.");
            }

            var parent = Directory.GetParent(current)?.FullName;
            if (string.IsNullOrEmpty(parent)
                || parent.Equals(current, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            current = Path.TrimEndingDirectorySeparator(parent);
        }
    }

    private static bool IsAtOrBelow(string candidate, string root)
    {
        var normalizedRoot = Path.TrimEndingDirectorySeparator(root);
        return candidate.Equals(normalizedRoot, StringComparison.OrdinalIgnoreCase)
            || candidate.StartsWith(
                normalizedRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase);
    }

    private sealed record Pointer(int SchemaVersion, string Root);
}
