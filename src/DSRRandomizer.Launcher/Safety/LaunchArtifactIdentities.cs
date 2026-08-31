using System.Reflection;

namespace DSRRandomizer.Launcher.Safety;

internal sealed record LaunchArtifactIdentities
{
    private const string GuardKey = "DSRRandomizer.GuardSha256";
    private const string ProfileKey = "DSRRandomizer.ProfileSha256";
    private const string BridgeKey = "DSRRandomizer.RmmBridgeSha256";
    private const string HostKey = "DSRRandomizer.RmmBridgeHostSha256";

    public LaunchArtifactIdentities(string guardSha256, string profileSha256)
        : this(guardSha256, profileSha256, new string('0', 64), new string('0', 64))
    {
    }

    public LaunchArtifactIdentities(
        string guardSha256,
        string profileSha256,
        string bridgeSha256,
        string hostSha256)
    {
        if (!IsSha256(guardSha256)
            || !IsSha256(profileSha256)
            || !IsSha256(bridgeSha256)
            || !IsSha256(hostSha256))
        {
            throw new ArgumentException("Launch artifact identities must be exact SHA-256 values.");
        }
        GuardSha256 = guardSha256.ToLowerInvariant();
        ProfileSha256 = profileSha256.ToLowerInvariant();
        BridgeSha256 = bridgeSha256.ToLowerInvariant();
        HostSha256 = hostSha256.ToLowerInvariant();
    }

    public string GuardSha256 { get; }
    public string ProfileSha256 { get; }
    public string BridgeSha256 { get; }
    public string HostSha256 { get; }

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
            || !metadata.TryGetValue(ProfileKey, out var profile)
            || !metadata.TryGetValue(BridgeKey, out var bridge)
            || !metadata.TryGetValue(HostKey, out var host))
        {
            throw new InvalidOperationException(
                "The launcher does not contain its build-pinned artifact identities.");
        }
        return new LaunchArtifactIdentities(guard, profile, bridge, host);
    }

    private static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(Uri.IsHexDigit);
}
