using System.Diagnostics;
using DSRRandomizer.Launcher.Configuration;

namespace DSRRandomizer.Launcher.Tests.Configuration;

public sealed class ExternalRootSelectionStoreTests
{
    [Fact]
    public async Task WriteAsync_StoresOnlyCanonicalExternalRoot()
    {
        using var fixture = SelectionFixture.Create();

        await fixture.Store.WriteAsync(fixture.ExternalRoot + Path.DirectorySeparatorChar, CancellationToken.None);

        Assert.Equal(fixture.ExternalRoot, await fixture.Store.ReadAsync(CancellationToken.None));
        Assert.Equal(new[] { "external-root.json" }, fixture.LocalFiles());
    }

    [Theory]
    [InlineData("relative-root")]
    [InlineData(".")]
    public async Task WriteAsync_RejectsNonAbsoluteOrFilesystemRoot(string suppliedRoot)
    {
        using var fixture = SelectionFixture.Create();

        await Assert.ThrowsAsync<ArgumentException>(() =>
            fixture.Store.WriteAsync(suppliedRoot, CancellationToken.None));
    }

    [Fact]
    public async Task WriteAsync_RejectsSourceInstallationDescendant()
    {
        using var fixture = SelectionFixture.Create();
        var descendant = Path.Combine(fixture.SourceRoot, "DSR-Modded");
        Directory.CreateDirectory(descendant);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(() =>
            fixture.Store.WriteAsync(descendant, CancellationToken.None));
    }

    [Fact]
    public async Task WriteAsync_RejectsExternalRootContainingSelectedSourceInstallation()
    {
        using var fixture = SelectionFixture.Create();
        var source = Path.Combine(fixture.ExternalRoot, "steam-source");
        Directory.CreateDirectory(source);
        var store = new ExternalRootSelectionStore(fixture.LocalRoot, source);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(() =>
            store.WriteAsync(fixture.ExternalRoot, CancellationToken.None));
    }

    [Fact]
    public async Task WriteAsync_RejectsSourceInstallationDescendantWithoutPriorSelection()
    {
        using var fixture = SelectionFixture.Create();
        File.WriteAllText(Path.Combine(fixture.SourceRoot, "DarkSoulsRemastered.exe"), "source game");
        var descendant = Path.Combine(fixture.SourceRoot, "DSR-Modded");
        Directory.CreateDirectory(descendant);
        var store = new ExternalRootSelectionStore(fixture.LocalRoot);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(() =>
            store.WriteAsync(descendant, CancellationToken.None));
    }

    [Fact]
    public async Task WriteAsync_RejectsNonexistentRoot()
    {
        using var fixture = SelectionFixture.Create();

        await Assert.ThrowsAsync<DirectoryNotFoundException>(() =>
            fixture.Store.WriteAsync(Path.Combine(fixture.Container, "missing"), CancellationToken.None));
    }

    [Fact]
    public async Task WriteAsync_RejectsReparseRoot()
    {
        using var fixture = SelectionFixture.Create();
        var target = Path.Combine(fixture.Container, "target");
        var link = Path.Combine(fixture.Container, "linked-root");
        Directory.CreateDirectory(target);
        CreateJunction(link, target);
        try
        {
            await Assert.ThrowsAsync<IOException>(() =>
                fixture.Store.WriteAsync(link, CancellationToken.None));
        }
        finally
        {
            DeleteJunction(link);
        }
    }

    [Fact]
    public async Task WriteAsync_RejectsIntermediateReparseSegment()
    {
        using var fixture = SelectionFixture.Create();
        var target = Path.Combine(fixture.Container, "target-parent");
        var link = Path.Combine(fixture.Container, "linked-parent");
        var leaf = Path.Combine(link, "leaf");
        Directory.CreateDirectory(Path.Combine(target, "leaf"));
        CreateJunction(link, target);
        try
        {
            await Assert.ThrowsAsync<IOException>(() =>
                fixture.Store.WriteAsync(leaf, CancellationToken.None));
        }
        finally
        {
            DeleteJunction(link);
        }
    }

    [Theory]
    [InlineData("{\"schemaVersion\":1,\"root\":\"C:\\\\External\",\"extra\":true}")]
    [InlineData("{\"schemaVersion\":1,\"schemaVersion\":1,\"root\":\"C:\\\\External\"}")]
    [InlineData("{\"schemaVersion\":1,\"root\":\"C:\\\\External\",\"root\":\"C:\\\\Other\"}")]
    [InlineData("{\"schemaVersion\":2,\"root\":\"C:\\\\External\"}")]
    [InlineData("{\"schemaVersion\":1.0,\"root\":\"C:\\\\External\"}")]
    [InlineData("{\"schemaVersion\":1}")]
    public async Task ReadAsync_RejectsMalformedDuplicateOrUnknownProperties(string json)
    {
        using var fixture = SelectionFixture.Create();
        await fixture.WritePointerAsync(json);

        await Assert.ThrowsAsync<IOException>(() => fixture.Store.ReadAsync(CancellationToken.None));
    }

    [Fact]
    public async Task ReadAsync_RejectsPointerToNonexistentExternalRoot()
    {
        using var fixture = SelectionFixture.Create();
        await fixture.WritePointerAsync("{\"schemaVersion\":1,\"root\":\"C:\\\\missing-external-root\"}");

        await Assert.ThrowsAsync<DirectoryNotFoundException>(() => fixture.Store.ReadAsync(CancellationToken.None));
    }

    private sealed class SelectionFixture : IDisposable
    {
        private SelectionFixture(string container)
        {
            Container = container;
            LocalRoot = Path.Combine(container, "local");
            SourceRoot = Path.Combine(container, "source");
            ExternalRoot = Path.Combine(container, "external");
            Directory.CreateDirectory(LocalRoot);
            Directory.CreateDirectory(SourceRoot);
            Directory.CreateDirectory(ExternalRoot);
            Store = new ExternalRootSelectionStore(LocalRoot, SourceRoot);
        }

        public string Container { get; }

        public string LocalRoot { get; }

        public string SourceRoot { get; }

        public string ExternalRoot { get; }

        public ExternalRootSelectionStore Store { get; }

        public static SelectionFixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-external-root-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new SelectionFixture(container);
        }

        public IReadOnlyList<string> LocalFiles() => Directory
            .EnumerateFiles(LocalRoot, "*", SearchOption.AllDirectories)
            .Select(path => Path.GetRelativePath(LocalRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        public Task WritePointerAsync(string json) => File.WriteAllTextAsync(
            Path.Combine(LocalRoot, "external-root.json"),
            json);

        public void Dispose() => Directory.Delete(Container, recursive: true);
    }

    private static void CreateJunction(string path, string target) => RunCmd($"mklink /J \"{path}\" \"{target}\"");

    private static void DeleteJunction(string path) => RunCmd($"rmdir \"{path}\"");

    private static void RunCmd(string command)
    {
        using var process = Process.Start(new ProcessStartInfo("cmd.exe", $"/d /c {command}")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            UseShellExecute = false
        }) ?? throw new IOException("Unable to start cmd.exe.");
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new IOException($"Command failed ({process.ExitCode}): {error}");
        }
    }
}
