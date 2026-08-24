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

    [Fact]
    public async Task Dispose_KillsFixtureAndConfirmedDescendantProcess()
    {
        var fixturePath = FindFixturePath();
        var platform = new WindowsProtectedProcessPlatform();
        var request = CreateRequest(fixturePath) with
        {
            Arguments = new[] { "--spawn-child" }
        };
        IProtectedProcess? process = null;
        var childProcessId = 0;
        string? childPidPath = null;

        try
        {
            process = await platform.CreateSuspendedAsync(request, CancellationToken.None);
            var parentProcessId = process.ProcessId;
            childPidPath = Path.Combine(
                Path.GetTempPath(),
                $"DSRRandomizerFixtureChild-{parentProcessId}.txt");
            File.Delete(childPidPath);
            process.AssignKillOnCloseJob();
            Assert.Equal(1u, process.ResumeMainThread());
            childProcessId = await WaitForChildProcessIdAsync(
                childPidPath,
                TimeSpan.FromSeconds(5));
            using (var childProcess = Process.GetProcessById(childProcessId))
            {
                Assert.False(childProcess.HasExited);
            }

            await process.DisposeAsync();
            process = null;

            Assert.True(await WaitForExitAsync(parentProcessId, TimeSpan.FromSeconds(5)));
            Assert.True(await WaitForExitAsync(childProcessId, TimeSpan.FromSeconds(5)));
        }
        finally
        {
            if (process is not null)
            {
                await process.DisposeAsync();
            }

            if (childProcessId != 0)
            {
                KillIfRunning(childProcessId);
            }

            if (childPidPath is not null)
            {
                File.Delete(childPidPath);
            }
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

    private static async Task<int> WaitForChildProcessIdAsync(
        string path,
        TimeSpan timeout)
    {
        var stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < timeout)
        {
            try
            {
                if (File.Exists(path) &&
                    int.TryParse(await File.ReadAllTextAsync(path), out var processId) &&
                    processId > 0)
                {
                    return processId;
                }
            }
            catch (IOException)
            {
                // The fixture may still be flushing its child-process id.
            }

            await Task.Delay(TimeSpan.FromMilliseconds(25));
        }

        throw new Xunit.Sdk.XunitException($"Fixture child PID was not reported: {path}");
    }
}
