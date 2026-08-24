using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Tests.Installation;

public sealed class InstallationSelectionStoreTests : IDisposable
{
    private readonly string _container = Path.Combine(
        Path.GetTempPath(),
        $"dsr-selection-{Guid.NewGuid():N}");

    [Fact]
    public async Task SaveAsync_AtomicallyPersistsCanonicalInstallationPath()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        Directory.CreateDirectory(source);
        Directory.CreateDirectory(local);
        var canonicalizer = new WindowsPathCanonicalizer();
        var boundary = WriteBoundary.Create(source, local, canonicalizer);
        var layout = LocalDataLayout.Create(local, boundary);
        var store = new InstallationSelectionStore(layout, boundary, canonicalizer);

        await store.SaveAsync(source, CancellationToken.None);
        var selected = await store.ReadAsync(CancellationToken.None);

        Assert.Equal(canonicalizer.Canonicalize(source), selected);
        Assert.Empty(Directory.EnumerateFiles(layout.Config, "*.tmp"));
    }

    [Fact]
    public async Task SaveAsync_DeniesConfigPathOutsideLocalRoot()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        var denied = Path.Combine(_container, "denied");
        Directory.CreateDirectory(source);
        Directory.CreateDirectory(local);
        var canonicalizer = new WindowsPathCanonicalizer();
        var boundary = WriteBoundary.Create(source, local, canonicalizer);
        var layout = LocalDataLayout.Create(local, boundary) with { Config = denied };
        var store = new InstallationSelectionStore(layout, boundary, canonicalizer);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(
            () => store.SaveAsync(source, CancellationToken.None));

        Assert.False(Directory.Exists(denied));
    }

    public void Dispose()
    {
        if (Directory.Exists(_container))
        {
            Directory.Delete(_container, recursive: true);
        }
    }
}
