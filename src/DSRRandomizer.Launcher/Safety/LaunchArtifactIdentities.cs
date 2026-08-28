using System.Reflection;

namespace DSRRandomizer.Launcher.Safety;

internal sealed record LaunchArtifactIdentities
{
    private const string GuardKey = "DSRRandomizer.GuardSha256";
    private const string ProfileKey = "DSRRandomizer.ProfileSha256";

    public LaunchArtifactIdentities(string guardSha256, string profileSha256)
    {
        if (!IsSha256(guardSha256) || !IsSha256(profileSha256))
        {
            throw new ArgumentException("Launch artifact identities must be exact SHA-256 values.");
        }
        GuardSha256 = guardSha256.ToLowerInvariant();
        ProfileSha256 = profileSha256.ToLowerInvariant();
    }

    public string GuardSha256 { get; }
    public string ProfileSha256 { get; }

    public static LaunchArtifactIdentities LoadEmbedded()
    {
        var metadata = typeof(LaunchArtifactIdentities).Assembly
            .GetCustomAttributes<AssemblyMetadataAttribute>()
            .Where(attribute => attribute.Value is not null)
            .ToDictionary(
                attribute => attribute.Key,
                attribute => attribute.Value!,
                StringComparer.Ordinal);
        if (!metadata.TryGetValue(GuardKey, out var guard)
            || !metadata.TryGetValue(ProfileKey, out var profile))
        {
            throw new InvalidOperationException(
                "The launcher does not contain its build-pinned artifact identities.");
        }
        return new LaunchArtifactIdentities(guard, profile);
    }

    private static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(Uri.IsHexDigit);
}
