using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Foundation.Tests.Saves;

public sealed class WindowsSaveProfileLocatorTests : IDisposable
{
    private readonly string _documents = Path.Combine(
        Path.GetTempPath(),
        $"dsr-save-discovery-{Guid.NewGuid():N}");

    public WindowsSaveProfileLocatorTests()
    {
        Directory.CreateDirectory(_documents);
        Locator = new WindowsSaveProfileLocator(new FixtureKnownFolderProvider(_documents));
    }

    private WindowsSaveProfileLocator Locator { get; }

    [Fact]
    public async Task DiscoverAsync_ReturnsOnlyNumericExactNormalSave()
    {
        File("12345678901234567/DRAKS0005.sl2");
        File("12345678901234567/DRAKS0005.sl2.overhaul.sl2");
        File("backup/DRAKS0005.sl2");

        var result = await Locator.DiscoverAsync(default);

        Assert.Collection(result, x => Assert.Equal("12345678901234567", x.SteamId));
    }

    [Fact]
    public async Task DiscoverAsync_ReturnsEmptyListWhenNoExactNormalSaveExists()
    {
        File("12345678901234567/DRAKS0005.sl2.overhaul.sl2");

        var result = await Locator.DiscoverAsync(default);

        Assert.Empty(result);
    }

    [Fact]
    public async Task DiscoverAsync_ReturnsCandidatesSortedBySteamIdWithoutSelectingOne()
    {
        File("99999999999999999/DRAKS0005.sl2");
        File("12345678901234567/DRAKS0005.sl2");
        File("not-a-steam-id/DRAKS0005.sl2");
        File("12345678901234567/nested/DRAKS0005.sl2");

        var result = await Locator.DiscoverAsync(default);

        Assert.Equal(
            ["12345678901234567", "99999999999999999"],
            result.Select(x => x.SteamId));
        Assert.All(result, x => Assert.EndsWith("DRAKS0005.sl2", x.SourcePath, StringComparison.OrdinalIgnoreCase));
    }

    public void Dispose()
    {
        if (Directory.Exists(_documents))
        {
            Directory.Delete(_documents, recursive: true);
        }
    }

    private void File(string relativePath)
    {
        var path = Path.Combine(
            _documents,
            "NBGI",
            "DARK SOULS REMASTERED",
            relativePath.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        System.IO.File.WriteAllText(path, "fixture");
    }

    private sealed class FixtureKnownFolderProvider(string documentsPath) : IKnownFolderProvider
    {
        public string GetDocumentsPath() => documentsPath;
    }
}
