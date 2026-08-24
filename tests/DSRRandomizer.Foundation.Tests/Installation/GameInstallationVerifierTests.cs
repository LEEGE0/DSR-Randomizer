using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Tests.Installation;

public sealed class GameInstallationVerifierTests
{
    [Fact]
    public async Task VerifyAsync_ExcludesInstalledModsAndCatalogsOnlyAllowedContent()
    {
        using var tree = FakeGameTree.CreateCompleteLayout();
        tree.Write("DarkSoulsRemastered.exe", "game");
        tree.Write("steam_api64.dll", "steam");
        tree.Write("map/MapStudio/m10_00_00_00.msb.dcx", "map");
        tree.Write("d3d11.dll", "overhaul loader");
        tree.Write("d3d11_mod.ini", "overhaul config");
        tree.Write("overhaul/GameParam.parambnd.dcx", "overhaul data");
        tree.Write("DSRQuickSummonCompanion.dll", "companion");
        tree.Write("crash/dump.dmp", "private crash data");
        tree.Write("error_reporter_creds.json", "credentials");

        var result = await CreateVerifier(tree.LocalDataRoot)
            .VerifyAsync(tree.Root, CancellationToken.None);

        Assert.True(result.IsValid, string.Join(Environment.NewLine, result.Errors));
        Assert.Equal(
            new[]
            {
                "DarkSoulsRemastered.exe",
                "map/MapStudio/m10_00_00_00.msb.dcx",
                "steam_api64.dll"
            },
            result.Catalog!.Files.Select(file => file.RelativePath));
        Assert.Equal(12, result.Catalog.TotalBytes);
    }

    [Fact]
    public async Task VerifyAsync_RejectsInstallationWithoutExecutable()
    {
        using var tree = FakeGameTree.CreateCompleteLayout();

        var result = await CreateVerifier(tree.LocalDataRoot)
            .VerifyAsync(tree.Root, CancellationToken.None);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, error => error.Contains("DarkSoulsRemastered.exe", StringComparison.Ordinal));
        Assert.Null(result.Catalog);
    }

    [Fact]
    public async Task VerifyAsync_RejectsInstallationWithoutRequiredDataDirectory()
    {
        using var tree = FakeGameTree.CreateCompleteLayout();
        tree.Write("DarkSoulsRemastered.exe", "game");
        Directory.Delete(tree.PathOf("sound"));

        var result = await CreateVerifier(tree.LocalDataRoot)
            .VerifyAsync(tree.Root, CancellationToken.None);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, error => error.Contains("sound", StringComparison.Ordinal));
    }

    [Fact]
    public async Task VerifyAsync_RejectsNonexistentInstallation()
    {
        using var tree = FakeGameTree.CreateCompleteLayout();
        var missing = tree.PathOf("does-not-exist");

        var result = await CreateVerifier(tree.LocalDataRoot)
            .VerifyAsync(missing, CancellationToken.None);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, error => error.Contains("does not exist", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task VerifyAsync_RejectsPathInsideRandomizerLocalDataRoot()
    {
        using var tree = FakeGameTree.CreateCompleteLayout();
        var copiedGame = tree.PathOfLocalData("runtimes/runtime-test");
        FakeGameTree.CreateLayoutAt(copiedGame);
        File.WriteAllText(Path.Combine(copiedGame, "DarkSoulsRemastered.exe"), "copy");

        var result = await CreateVerifier(tree.LocalDataRoot)
            .VerifyAsync(copiedGame, CancellationToken.None);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, error => error.Contains("local-data root", StringComparison.OrdinalIgnoreCase));
    }

    private static GameInstallationVerifier CreateVerifier(string localDataRoot) =>
        new(new WindowsPathCanonicalizer(), localDataRoot);

    private sealed class FakeGameTree : IDisposable
    {
        private static readonly string[] RequiredDirectories =
        {
            "chr", "event", "facegen", "font", "map", "menu", "movww", "msg",
            "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
            "sfx", "shader", "sound"
        };

        private readonly string _container;

        private FakeGameTree(string container)
        {
            _container = container;
            Root = Path.Combine(container, "game");
            LocalDataRoot = Path.Combine(container, "local-data");
        }

        public string Root { get; }

        public string LocalDataRoot { get; }

        public static FakeGameTree CreateCompleteLayout()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-install-{Guid.NewGuid():N}");
            var tree = new FakeGameTree(container);
            CreateLayoutAt(tree.Root);
            Directory.CreateDirectory(tree.LocalDataRoot);
            return tree;
        }

        public static void CreateLayoutAt(string root)
        {
            Directory.CreateDirectory(root);
            foreach (var directory in RequiredDirectories)
            {
                Directory.CreateDirectory(Path.Combine(root, directory));
            }
        }

        public string PathOf(string relativePath) =>
            Path.Combine(Root, relativePath.Replace('/', Path.DirectorySeparatorChar));

        public string PathOfLocalData(string relativePath) =>
            Path.Combine(LocalDataRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));

        public void Write(string relativePath, string content)
        {
            var path = PathOf(relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
        }

        public void Dispose() => Directory.Delete(_container, recursive: true);
    }
}
