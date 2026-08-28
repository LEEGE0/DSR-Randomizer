using System.Diagnostics;
using System.Security.Cryptography;

namespace DSRRandomizer.Launcher.Tests;

public sealed class PinnedArtifactPublishTests : IDisposable
{
    private readonly string _publishRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-reused-publish-{Guid.NewGuid():N}");
    private readonly string _packageOutputRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-reused-package-{Guid.NewGuid():N}");

    [Fact]
    public async Task ReusedPublishReplacesFutureDatedTamperedPinnedArtifacts()
    {
        var repositoryRoot = FindRepositoryRoot();
        await PublishAsync(repositoryRoot);
        var publishedGuard = Path.Combine(
            _publishRoot,
            "native",
            "DSRRandomizer.Runtime.dll");
        var publishedProfile = Path.Combine(
            _publishRoot,
            "config",
            "compatibility-profiles.json");
        await File.WriteAllTextAsync(publishedGuard, "future-dated tampered guard");
        await File.WriteAllTextAsync(publishedProfile, "future-dated tampered profile");
        var future = DateTime.UtcNow.AddYears(1);
        File.SetLastWriteTimeUtc(publishedGuard, future);
        File.SetLastWriteTimeUtc(publishedProfile, future);

        var rejectedPackage = await PackageAsync(repositoryRoot);

        Assert.NotEqual(0, rejectedPackage.ExitCode);
        Assert.Contains(
            "Release-content validation failed",
            rejectedPackage.Output,
            StringComparison.Ordinal);

        await PublishAsync(repositoryRoot);

        Assert.Equal(
            Sha256(Path.Combine(
                repositoryRoot,
                "native",
                "out",
                "build",
                "windows-x64-release",
                "native",
                "runtime",
                "Release",
                "DSRRandomizer.Runtime.dll")),
            Sha256(publishedGuard));
        Assert.Equal(
            Sha256(Path.Combine(repositoryRoot, "config", "compatibility-profiles.json")),
            Sha256(publishedProfile));
    }

    private async Task<(int ExitCode, string Output)> PackageAsync(string repositoryRoot)
    {
        var startInfo = new ProcessStartInfo("pwsh.exe")
        {
            WorkingDirectory = repositoryRoot,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in new[]
                 {
                     "-NoProfile",
                     "-ExecutionPolicy",
                     "Bypass",
                     "-File",
                     Path.Combine(repositoryRoot, "packaging", "package.ps1"),
                     "-Version",
                     "round2-reused-publish-test",
                     "-PublishPath",
                     _publishRoot,
                     "-OutputPath",
                     _packageOutputRoot
                 })
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Unable to start package.ps1.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (
            process.ExitCode,
            $"{await outputTask}\n{await errorTask}");
    }

    private async Task PublishAsync(string repositoryRoot)
    {
        var startInfo = new ProcessStartInfo("dotnet")
        {
            WorkingDirectory = repositoryRoot,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in new[]
                 {
                     "publish",
                     "src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj",
                     "-c",
                     "Release",
                     "-r",
                     "win-x64",
                     "--self-contained",
                     "true",
                     "--no-restore",
                     "-o",
                     _publishRoot
                 })
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Unable to start dotnet publish.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var output = await outputTask;
        var error = await errorTask;
        Assert.True(
            process.ExitCode == 0,
            $"dotnet publish failed with exit code {process.ExitCode}.\n{output}\n{error}");
    }

    private static string Sha256(string path) => Convert.ToHexString(
        SHA256.HashData(File.ReadAllBytes(path)));

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, ".git"))
                || Directory.Exists(Path.Combine(current.FullName, ".git")))
            {
                return current.FullName;
            }
            current = current.Parent;
        }
        throw new DirectoryNotFoundException("Unable to locate the repository root.");
    }

    public void Dispose()
    {
        if (Directory.Exists(_publishRoot))
        {
            Directory.Delete(_publishRoot, recursive: true);
        }
        if (Directory.Exists(_packageOutputRoot))
        {
            Directory.Delete(_packageOutputRoot, recursive: true);
        }
    }
}
