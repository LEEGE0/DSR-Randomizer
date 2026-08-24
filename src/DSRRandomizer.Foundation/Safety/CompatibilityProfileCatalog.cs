namespace DSRRandomizer.Foundation.Safety;

public sealed class CompatibilityProfileCatalog
{
    private const string SupportedHash =
        "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b";

    private readonly IReadOnlyDictionary<ExecutableKey, CompatibilityProfile> _profiles;

    public CompatibilityProfileCatalog(IEnumerable<CompatibilityProfile> profiles)
    {
        ArgumentNullException.ThrowIfNull(profiles);

        var byIdentity = new Dictionary<ExecutableKey, CompatibilityProfile>();
        foreach (var profile in profiles)
        {
            ArgumentNullException.ThrowIfNull(profile);
            var key = CreateKey(profile.Executable);
            if (!byIdentity.TryAdd(key, profile))
            {
                throw new ArgumentException(
                    "Compatibility profiles contain a duplicate executable identity.",
                    nameof(profiles));
            }
        }

        _profiles = byIdentity;
    }

    public static CompatibilityProfileCatalog Default { get; } = new(
        new[]
        {
            new CompatibilityProfile(
                "dsr-steam-a45aaa36",
                new ExecutableIdentity(
                    50286344,
                    SupportedHash,
                    0x8664,
                    0x6344ca56,
                    52015104),
                4326608,
                1)
        });

    public CompatibilityProfile Select(ExecutableIdentity executable)
    {
        ArgumentNullException.ThrowIfNull(executable);
        var key = CreateKey(executable);
        if (_profiles.TryGetValue(key, out var profile))
        {
            return profile;
        }

        throw new UnsupportedGameBuildException(
            $"The Dark Souls Remastered build is unsupported: {key.Sha256}.");
    }

    private static ExecutableKey CreateKey(ExecutableIdentity executable)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executable.Sha256);
        if (executable.Sha256.Length != 64 || executable.Sha256.Any(character => !Uri.IsHexDigit(character)))
        {
            throw new ArgumentException(
                "Executable SHA-256 must contain exactly 64 hexadecimal characters.",
                nameof(executable));
        }

        return new ExecutableKey(
            executable.Length,
            executable.Sha256.ToLowerInvariant(),
            executable.Machine,
            executable.PeTimestamp,
            executable.SizeOfImage);
    }

    private sealed record ExecutableKey(
        long Length,
        string Sha256,
        ushort Machine,
        uint PeTimestamp,
        uint SizeOfImage);
}
