using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Foundation.Tests.Saves;

public sealed class SaveSelectionStoreTests : IDisposable
{
    private readonly string _container = Path.Combine(
        Path.GetTempPath(),
        $"dsr-save-selection-{Guid.NewGuid():N}");

    [Fact]
    public async Task WriteAsync_PersistsOnlySteamIdAndCanonicalExactSourcePath()
    {
        var source = Path.Combine(_container, "documents", "NBGI", "DARK SOULS REMASTERED", "12345678901234567", "DRAKS0005.sl2");
        var (layout, boundary, canonicalizer) = CreateExternalLayout();
        Directory.CreateDirectory(Path.GetDirectoryName(source)!);
        System.IO.File.WriteAllText(source, "fixture");
        var store = new SaveSelectionStore(layout, boundary, canonicalizer);

        await store.WriteAsync(new SaveProfileCandidate("12345678901234567", source), default);
        var selection = await store.ReadAsync(default);
        var document = JsonDocument.Parse(await System.IO.File.ReadAllTextAsync(Path.Combine(layout.Config, "selected-save-profile.json")));

        Assert.Equal(new SaveProfileCandidate("12345678901234567", canonicalizer.Canonicalize(source)), selection);
        Assert.Equal(["sourcePath", "steamId"], document.RootElement.EnumerateObject().Select(x => x.Name).Order());
    }

    [Fact]
    public async Task WriteAsync_DeniesPersistenceOutsideExternalDataRoot()
    {
        var source = Path.Combine(_container, "documents", "NBGI", "DARK SOULS REMASTERED", "12345678901234567", "DRAKS0005.sl2");
        var (layout, boundary, canonicalizer) = CreateExternalLayout();
        Directory.CreateDirectory(Path.GetDirectoryName(source)!);
        System.IO.File.WriteAllText(source, "fixture");
        var deniedLayout = layout with { Config = Path.Combine(_container, "denied") };
        var store = new SaveSelectionStore(deniedLayout, boundary, canonicalizer);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(
            () => store.WriteAsync(new SaveProfileCandidate("12345678901234567", source), default));

        Assert.False(Directory.Exists(deniedLayout.Config));
    }

    [Fact]
    public async Task WriteAsync_RejectsANonExactNormalSavePath()
    {
        var source = Path.Combine(_container, "documents", "NBGI", "DARK SOULS REMASTERED", "12345678901234567", "DRAKS0005.sl2.overhaul.sl2");
        var (layout, boundary, canonicalizer) = CreateExternalLayout();
        Directory.CreateDirectory(Path.GetDirectoryName(source)!);
        System.IO.File.WriteAllText(source, "fixture");
        var store = new SaveSelectionStore(layout, boundary, canonicalizer);

        await Assert.ThrowsAsync<ArgumentException>(
            () => store.WriteAsync(new SaveProfileCandidate("12345678901234567", source), default));

        Assert.False(Directory.Exists(layout.Config));
    }

    [Fact]
    public async Task ReadAsync_ReturnsNullWhenNoSelectionWasWritten()
    {
        var (layout, boundary, canonicalizer) = CreateExternalLayout();
        var store = new SaveSelectionStore(layout, boundary, canonicalizer);

        var selection = await store.ReadAsync(default);

        Assert.Null(selection);
    }

    public void Dispose()
    {
        if (Directory.Exists(_container))
        {
            Directory.Delete(_container, recursive: true);
        }
    }

    private (LocalDataLayout Layout, WriteBoundary Boundary, IPathCanonicalizer Canonicalizer) CreateExternalLayout()
    {
        var source = Path.Combine(_container, "source-installation");
        var local = Path.Combine(_container, "local-data");
        Directory.CreateDirectory(source);
        Directory.CreateDirectory(local);
        var canonicalizer = new WindowsPathCanonicalizer();
        var boundary = WriteBoundary.Create(source, local, canonicalizer);
        return (LocalDataLayout.Create(local, boundary), boundary, canonicalizer);
    }
}
