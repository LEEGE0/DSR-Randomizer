using System.Text.Json;
using System.Text.Json.Serialization;

namespace DSRRandomizer.Foundation.Runtime;

public sealed record RuntimeFileManifestEntry(
    string RelativePath,
    long Length,
    string Sha256);

public sealed record RuntimeManifest(
    int SchemaVersion,
    string RuntimeId,
    DateTimeOffset CreatedAtUtc,
    string SourceExecutableSha256,
    string CatalogSha256,
    long TotalBytes,
    IReadOnlyList<RuntimeFileManifestEntry> Files)
{
    [JsonIgnore]
    public string RuntimePath { get; init; } = string.Empty;
}

public sealed record RuntimeBuildProgress(
    long CopiedBytes,
    long TotalBytes,
    string RelativePath);

internal static class RuntimeJson
{
    public static JsonSerializerOptions Options { get; } = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false
    };
}
