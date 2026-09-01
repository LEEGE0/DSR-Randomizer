using System.Diagnostics;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests.Services;

public sealed class RandomizerProcessPlatformTests
{
    [Fact]
    public async Task LaunchModEngineAsync_CancellationAfterCreationWaitsForBootstrapExit()
    {
        var powershell = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell",
            "v1.0",
            "powershell.exe");
        var request = new RandomizerProcessRequest(
            powershell,
            Path.GetTempPath(),
            ["-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Milliseconds 400"]);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
        var stopwatch = Stopwatch.StartNew();

        var result = await new WindowsRandomizerProcessPlatform().LaunchModEngineAsync(
            request,
            cancellation.Token);

        stopwatch.Stop();
        Assert.True(result.Started, result.ErrorCode);
        Assert.True(stopwatch.Elapsed >= TimeSpan.FromMilliseconds(300), stopwatch.Elapsed.ToString());
    }
}
