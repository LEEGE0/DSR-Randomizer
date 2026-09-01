using System.Security.Cryptography;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class LaunchArtifactLeaseTests
{
    [Fact]
    public async Task TryOpen_AcceptsRegularFileBeyondLegacyMaxPath()
    {
        var fixtureRoot = Path.Combine(
            Path.GetTempPath(),
            $"dsr-long-artifact-{Guid.NewGuid():N}");
        var longParent = Path.Combine(
            fixtureRoot,
            new string('a', 80),
            new string('b', 80),
            new string('c', 80));
        var artifactPath = Path.Combine(longParent, "artifact.bin");
        var expectedBytes = "long-path-launch-artifact"u8.ToArray();

        try
        {
            Directory.CreateDirectory(longParent);
            await File.WriteAllBytesAsync(artifactPath, expectedBytes);
            Assert.True(artifactPath.Length > 260, artifactPath);

            using var lease = LaunchArtifactLease.TryOpen(artifactPath);

            Assert.NotNull(lease);
            Assert.Equal(expectedBytes, lease.Bytes);
            Assert.Equal(
                Convert.ToHexString(SHA256.HashData(expectedBytes)).ToLowerInvariant(),
                lease.Sha256);
        }
        finally
        {
            if (Directory.Exists(fixtureRoot))
            {
                Directory.Delete(fixtureRoot, recursive: true);
            }
        }
    }
}
