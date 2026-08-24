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
    public void Validate_RejectsEveryUnrecognizedOrGameDerivedPath(string path)
    {
        Assert.Contains(path, new ReleaseContentGuard().Validate(new[] { path }));
    }

    [Fact]
    public void Validate_AllowsOnlyPublishedLauncherAndProjectNotices()
    {
        var paths = new[]
        {
            "DSRRandomizer.Launcher.exe",
            "DSRRandomizer.Launcher.pdb",
            "README.md",
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "CHANGELOG.md"
        };

        Assert.Empty(new ReleaseContentGuard().Validate(paths));
    }

    [Fact]
    public void Validate_RejectsDuplicatePathsIgnoringWindowsCase()
    {
        var paths = new[] { "README.md", "readme.md" };

        Assert.Equal(paths, new ReleaseContentGuard().Validate(paths));
    }
}
