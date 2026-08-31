using DSRRandomizer.Foundation.Packaging;

namespace DSRRandomizer.Foundation.Tests.Packaging;

public sealed class ReleaseContentGuardTests
{
    [Theory]
    [InlineData("DarkSoulsRemastered.exe")]
    [InlineData("DSRForMod.Launcher.pdb")]
    [InlineData("components/rmm-bridge/trace.pdb")]
    [InlineData("DRAKS0005.sl2")]
    [InlineData("DRAKS0005.rmm")]
    [InlineData("DS1EnemyRandomizer.exe")]
    [InlineData("DarkSoulsItemRandomizer.exe")]
    [InlineData("modengine2_launcher.exe")]
    [InlineData("DS1HeapPatch.dll")]
    [InlineData("seed/result.txt")]
    [InlineData("spoiler/result.txt")]
    [InlineData("logs/launcher.log")]
    [InlineData("profile/player.json")]
    [InlineData("saves/DRAKS0005.sl2")]
    [InlineData("staging/package.zip")]
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
    [InlineData(" README.md")]
    [InlineData("README.md ")]
    public void Validate_RejectsEveryUnrecognizedOrGameDerivedPath(string path)
    {
        Assert.Contains(path, new ReleaseContentGuard().Validate(new[] { path }));
    }

    [Fact]
    public void Validate_AllowsOnlyTheTwelvePublishedReleaseArtifacts()
    {
        var paths = new[]
        {
            "DSRForMod.Launcher.exe",
            "README.md",
            "INSTALL_KO.md",
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "CHANGELOG.md",
            "native/DSRRandomizer.Runtime.dll",
            "native/DSRRandomizer.Runtime.dll.sha256",
            "config/compatibility-profiles.json",
            "components/rmm-bridge/DSRRandomizer.RmmBridge.dll",
            "components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe",
            "components/rmm-bridge/deployment-manifest.json"
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
