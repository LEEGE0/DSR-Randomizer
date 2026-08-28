using DSRRandomizer.Foundation.Packaging;

namespace DSRRandomizer.Foundation.Tests.Packaging;

public sealed class ReleaseContentGuardTests
{
    [Theory]
    [InlineData("DarkSoulsRemastered.exe")]
    [InlineData("runtime/map/m10_00_00_00.msb.dcx")]
    [InlineData("saves/DRAKS-RANDOM.rsl2")]
    [InlineData("local-data/game-catalog.json")]
    [InlineData("DSRRandomizer.Foundation.dll")]
    [InlineData("../outside.txt")]
    [InlineData("nested/README.md")]
    [InlineData("Mods/EnemyRandomizer/config.ini")]
    [InlineData("captures/local.bin")]
    [InlineData("credentials.json")]
    [InlineData("config/generated-compatibility-profiles.json")]
    public void Validate_RejectsEveryUnrecognizedOrGameDerivedPath(string path)
    {
        Assert.Contains(path, new ReleaseContentGuard().Validate(new[] { path }));
    }

    [Fact]
    public void Validate_AllowsOnlyPublishedLauncherProjectGuardProfileAndNotices()
    {
        var paths = new[]
        {
            "DSRRandomizer.Launcher.exe",
            "DSRRandomizer.Launcher.pdb",
            "README.md",
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "CHANGELOG.md",
            "native/DSRRandomizer.Runtime.dll",
            "native/DSRRandomizer.Runtime.dll.sha256",
            "config/compatibility-profiles.json"
        };

        Assert.Empty(new ReleaseContentGuard().Validate(paths));
    }

    [Fact]
    public void Validate_RejectsDuplicatePathsIgnoringWindowsCase()
    {
        var paths = new[] { "README.md", "readme.md" };

        var failures = new ReleaseContentGuard().Validate(paths);
        Assert.Contains(paths[0], failures);
        Assert.Contains(paths[1], failures);
    }
}
