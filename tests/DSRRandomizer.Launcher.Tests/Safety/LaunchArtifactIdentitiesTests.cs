using System.Security.Cryptography;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class LaunchArtifactIdentitiesTests
{
    [Fact]
    public void LoadEmbedded_MatchesTheBuildInputsCopiedBesideTheLauncher()
    {
        var identities = LaunchArtifactIdentities.LoadEmbedded();

        Assert.Equal(
            Hash(Path.Combine(AppContext.BaseDirectory, "native", "DSRRandomizer.Runtime.dll")),
            identities.GuardSha256);
        Assert.Equal(
            Hash(Path.Combine(AppContext.BaseDirectory, "config", "compatibility-profiles.json")),
            identities.ProfileSha256);
    }

    private static string Hash(string path) => Convert.ToHexString(
        SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
}
