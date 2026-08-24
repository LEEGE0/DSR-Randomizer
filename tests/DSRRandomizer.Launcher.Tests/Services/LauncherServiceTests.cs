using System.Security.Cryptography;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests.Services;

public sealed class LauncherServiceTests : IDisposable
{
    private static readonly string[] RequiredDirectories =
    {
        "chr", "event", "facegen", "font", "map", "menu", "movww", "msg",
        "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
        "sfx", "shader", "sound"
    };

    private readonly string _container = Path.Combine(
        Path.GetTempPath(),
        $"dsr-launcher-service-{Guid.NewGuid():N}");

    [Fact]
    public async Task InitializeRuntimeAsync_PersistsSelectionForFreshStatusServiceWithoutChangingSource()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        CreateFakeInstallation(source);
        Directory.CreateDirectory(local);
        var before = CaptureSource(source);
        var service = new LauncherService(local);

        var manifest = await service.InitializeRuntimeAsync(
            source,
            progress: null,
            CancellationToken.None);
        var readinessFromFreshService = await new LauncherService(local)
            .GetReadinessAsync(CancellationToken.None);

        Assert.True(readinessFromFreshService.IsReady, string.Join(Environment.NewLine, readinessFromFreshService.Errors));
        Assert.Equal(manifest.RuntimePath, readinessFromFreshService.RuntimePath);
        Assert.False(File.Exists(Path.Combine(manifest.RuntimePath, "d3d11.dll")));
        Assert.Equal(before, CaptureSource(source));
    }

    public void Dispose()
    {
        if (Directory.Exists(_container))
        {
            Directory.Delete(_container, recursive: true);
        }
    }

    private static void CreateFakeInstallation(string source)
    {
        Directory.CreateDirectory(source);
        foreach (var directory in RequiredDirectories)
        {
            Directory.CreateDirectory(Path.Combine(source, directory));
        }

        File.WriteAllText(Path.Combine(source, "DarkSoulsRemastered.exe"), "game");
        File.WriteAllText(Path.Combine(source, "map", "test.dcx"), "map");
        File.WriteAllText(Path.Combine(source, "d3d11.dll"), "installed overhaul loader");
    }

    private static string CaptureSource(string source) => string.Join(
        Environment.NewLine,
        Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)
            .Order(StringComparer.Ordinal)
            .Select(path =>
            {
                var info = new FileInfo(path);
                var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
                return $"{Path.GetRelativePath(source, path)}|{info.Length}|{info.LastWriteTimeUtc.Ticks}|{hash}";
            }));
}
