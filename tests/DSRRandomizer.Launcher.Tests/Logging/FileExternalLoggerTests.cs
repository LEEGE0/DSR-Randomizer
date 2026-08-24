using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Launcher.Logging;

namespace DSRRandomizer.Launcher.Tests.Logging;

public sealed class FileExternalLoggerTests : IDisposable
{
    private readonly string _container = Path.Combine(
        Path.GetTempPath(),
        $"dsr-logger-{Guid.NewGuid():N}");

    [Fact]
    public async Task LogExceptionAsync_WritesStackTraceOnlyBelowExternalLogsRoot()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        Directory.CreateDirectory(source);
        Directory.CreateDirectory(local);
        var canonicalizer = new WindowsPathCanonicalizer();
        var boundary = WriteBoundary.Create(source, local, canonicalizer);
        var layout = LocalDataLayout.Create(local, boundary);
        var logger = new FileExternalLogger(layout, boundary);

        await logger.LogExceptionAsync(new IOException("copy failed"), CancellationToken.None);

        var logPath = Path.Combine(layout.Logs, "launcher.log");
        Assert.True(File.Exists(logPath));
        Assert.Contains("System.IO.IOException: copy failed", await File.ReadAllTextAsync(logPath));
        Assert.Empty(Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories));
    }

    [Fact]
    public async Task LogExceptionAsync_DeniesLogsPathOutsideLocalRoot()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        var denied = Path.Combine(_container, "denied-logs");
        Directory.CreateDirectory(source);
        Directory.CreateDirectory(local);
        var canonicalizer = new WindowsPathCanonicalizer();
        var boundary = WriteBoundary.Create(source, local, canonicalizer);
        var layout = LocalDataLayout.Create(local, boundary) with { Logs = denied };
        var logger = new FileExternalLogger(layout, boundary);

        await Assert.ThrowsAsync<UnauthorizedAccessException>(
            () => logger.LogExceptionAsync(new IOException("copy failed"), CancellationToken.None));

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
