using System.Diagnostics;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class WindowsJobObjectIntegrationTests
{
    [Fact]
    public async Task Dispose_KillsSuspendedFixtureAssignedToJob()
    {
        var fixturePath = FindFixturePath();
        var platform = new WindowsProtectedProcessPlatform();
        IProtectedProcess? process = null;

        try
        {
            process = await platform.CreateSuspendedAsync(
                CreateRequest(fixturePath),
                CancellationToken.None);
            var processId = process.ProcessId;
            process.AssignKillOnCloseJob();

            await process.DisposeAsync();
            process = null;

            Assert.True(await WaitForExitAsync(processId, TimeSpan.FromSeconds(5)));
        }
        finally
        {
            if (process is not null)
            {
                await process.DisposeAsync();
            }
        }
    }

    [Fact]
    public async Task Dispose_KillsSuspendedFixtureBeforeJobAssignment()
    {
        var fixturePath = FindFixturePath();
        var platform = new WindowsProtectedProcessPlatform();
        var process = await platform.CreateSuspendedAsync(
            CreateRequest(fixturePath),
            CancellationToken.None);
        var processId = process.ProcessId;

        try
        {
            await process.DisposeAsync();

            Assert.True(await WaitForExitAsync(processId, TimeSpan.FromMilliseconds(500)));
        }
        finally
        {
            KillIfRunning(processId);
            await process.DisposeAsync();
        }
    }

    private static SafetyLaunchRequest CreateRequest(string fixturePath) => new(
        fixturePath,
        Path.GetDirectoryName(fixturePath)!,
        fixturePath,
        CompatibilityProfileCatalog.Default.Select(new ExecutableIdentity(
            50286344,
            "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b",
            0x8664,
            0x6344ca56,
            52015104)),
        RequiredProtectionFlags: 0,
        DiagnosticMode: true);

    private static string FindFixturePath()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null &&
               !File.Exists(Path.Combine(directory.FullName, "DSR-Randomizer.sln")))
        {
            directory = directory.Parent;
        }

        Assert.NotNull(directory);
        var configuration = Directory.Exists(Path.Combine(
            directory.FullName,
            "native",
            "out",
            "build",
            "windows-x64-release"))
            ? "release"
            : "debug";
        var fixturePath = Path.Combine(
            directory.FullName,
            "native",
            "out",
            "build",
            $"windows-x64-{configuration}",
            "native",
            char.ToUpperInvariant(configuration[0]) + configuration[1..],
            "DSRRandomizer.SuspendedFixture.exe");

        Assert.True(File.Exists(fixturePath), $"Native fixture is missing: {fixturePath}");
        return fixturePath;
    }

    private static async Task<bool> WaitForExitAsync(int processId, TimeSpan timeout)
    {
        var stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < timeout)
        {
            try
            {
                using var process = Process.GetProcessById(processId);
                if (process.HasExited)
                {
                    return true;
                }
            }
            catch (ArgumentException)
            {
                return true;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(25));
        }

        return false;
    }

    private static void KillIfRunning(int processId)
    {
        try
        {
            using var process = Process.GetProcessById(processId);
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit();
            }
        }
        catch (ArgumentException)
        {
            // The fixture already exited and Windows released its process id.
        }
    }
}
