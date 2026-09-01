using System.Diagnostics;
using System.Text.Json;

namespace DSRRandomizer.Launcher.Tests;

public sealed class ReleaseSourceStateTests : IDisposable
{
    private readonly string _testRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-release-source-state-{Guid.NewGuid():N}");

    [Theory]
    [InlineData("clean", null)]
    [InlineData("dirty-main", "Main repository has tracked or nonignored untracked changes")]
    [InlineData("untracked-main", "src/InjectedCompileInput.cs")]
    [InlineData("missing-submodule", "Submodule 'third_party/dependency' is not initialized")]
    [InlineData("wrong-submodule-head", "Submodule 'third_party/dependency' is at the wrong commit")]
    [InlineData("dirty-submodule", "Submodule 'third_party/dependency' has tracked or nonignored untracked changes")]
    public async Task ReleaseSourceStateFailsClosedUnlessCommittedTreesExactlyMatch(
        string state,
        string? expectedFailure)
    {
        var fixture = await CreateFixtureAsync(state);
        var result = await AssertReleaseSourceStateAsync(fixture);

        if (expectedFailure is null)
        {
            Assert.True(
                result.ExitCode == 0,
                $"Clean exact release state was rejected with exit code {result.ExitCode}.\n{result.Output}");
            Assert.Contains(fixture.MainRevision, result.Output, StringComparison.Ordinal);
            Assert.Contains(fixture.SubmoduleRevision, result.Output, StringComparison.Ordinal);
        }
        else
        {
            Assert.NotEqual(0, result.ExitCode);
            Assert.Contains(expectedFailure, result.Output, StringComparison.Ordinal);
        }
    }

    private async Task<Fixture> CreateFixtureAsync(string state)
    {
        var fixtureRoot = Path.Combine(_testRoot, state);
        var upstreamRoot = Path.Combine(fixtureRoot, "upstream");
        var repositoryRoot = Path.Combine(fixtureRoot, "repository");
        Directory.CreateDirectory(upstreamRoot);
        Directory.CreateDirectory(repositoryRoot);

        await GitAsync(upstreamRoot, "init");
        await ConfigureIdentityAsync(upstreamRoot);
        await File.WriteAllTextAsync(Path.Combine(upstreamRoot, "LICENSE"), "fixture license\n");
        await File.WriteAllTextAsync(Path.Combine(upstreamRoot, "source.c"), "int fixture(void) { return 1; }\n");
        await GitAsync(upstreamRoot, "add", "LICENSE", "source.c");
        await GitAsync(upstreamRoot, "commit", "-m", "fixture dependency");
        var submoduleRevision = (await GitAsync(upstreamRoot, "rev-parse", "HEAD")).Trim();

        await GitAsync(repositoryRoot, "init");
        await ConfigureIdentityAsync(repositoryRoot);
        Directory.CreateDirectory(Path.Combine(repositoryRoot, "src"));
        await File.WriteAllTextAsync(Path.Combine(repositoryRoot, ".gitignore"), "artifacts/\nbin/\nobj/\n");
        await File.WriteAllTextAsync(Path.Combine(repositoryRoot, "README.md"), "fixture repository\n");
        await File.WriteAllTextAsync(
            Path.Combine(repositoryRoot, "src", "Program.cs"),
            "internal static class Program { }\n");
        await GitAsync(repositoryRoot, "add", ".gitignore", "README.md", "src/Program.cs");
        await GitAsync(repositoryRoot, "commit", "-m", "fixture repository");
        await GitAsync(
            repositoryRoot,
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            upstreamRoot,
            "third_party/dependency");
        await GitAsync(repositoryRoot, "commit", "-am", "pin fixture dependency");
        var mainRevision = (await GitAsync(repositoryRoot, "rev-parse", "HEAD")).Trim();

        var ignoredArtifact = Path.Combine(repositoryRoot, "artifacts", "ignored.zip");
        var ignoredBin = Path.Combine(repositoryRoot, "src", "bin", "ignored.dll");
        var ignoredObj = Path.Combine(repositoryRoot, "src", "obj", "ignored.cache");
        Directory.CreateDirectory(Path.GetDirectoryName(ignoredArtifact)!);
        Directory.CreateDirectory(Path.GetDirectoryName(ignoredBin)!);
        Directory.CreateDirectory(Path.GetDirectoryName(ignoredObj)!);
        await File.WriteAllTextAsync(ignoredArtifact, "ignored\n");
        await File.WriteAllTextAsync(ignoredBin, "ignored\n");
        await File.WriteAllTextAsync(ignoredObj, "ignored\n");

        var submoduleRoot = Path.Combine(repositoryRoot, "third_party", "dependency");
        switch (state)
        {
            case "clean":
                break;
            case "dirty-main":
                await File.AppendAllTextAsync(Path.Combine(repositoryRoot, "README.md"), "dirty\n");
                break;
            case "untracked-main":
                await File.WriteAllTextAsync(
                    Path.Combine(repositoryRoot, "src", "InjectedCompileInput.cs"),
                    "internal static class InjectedCompileInput { }\n");
                break;
            case "missing-submodule":
                Directory.Delete(submoduleRoot, recursive: true);
                break;
            case "wrong-submodule-head":
                await File.AppendAllTextAsync(Path.Combine(upstreamRoot, "source.c"), "int changed(void) { return 2; }\n");
                await GitAsync(upstreamRoot, "add", "source.c");
                await GitAsync(upstreamRoot, "commit", "-m", "second dependency revision");
                var wrongRevision = (await GitAsync(upstreamRoot, "rev-parse", "HEAD")).Trim();
                await GitAsync(submoduleRoot, "fetch", "origin");
                await GitAsync(submoduleRoot, "checkout", "--detach", wrongRevision);
                break;
            case "dirty-submodule":
                await File.AppendAllTextAsync(Path.Combine(submoduleRoot, "LICENSE"), "dirty\n");
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(state), state, null);
        }

        var contractPath = Path.Combine(fixtureRoot, "contract.json");
        await File.WriteAllTextAsync(
            contractPath,
            JsonSerializer.Serialize(new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["third_party/dependency"] = submoduleRevision
            }));
        return new Fixture(repositoryRoot, contractPath, mainRevision, submoduleRevision);
    }

    private static async Task ConfigureIdentityAsync(string repositoryRoot)
    {
        await GitAsync(repositoryRoot, "config", "user.name", "Release State Test");
        await GitAsync(repositoryRoot, "config", "user.email", "release-state@example.invalid");
        await GitAsync(repositoryRoot, "config", "core.autocrlf", "false");
    }

    private static async Task<(int ExitCode, string Output)> AssertReleaseSourceStateAsync(Fixture fixture)
    {
        var repositoryRoot = FindRepositoryRoot();
        var command = string.Join(
            "; ",
            "$ErrorActionPreference = 'Stop'",
            "Import-Module $env:DSR_RELEASE_STATE_MODULE -Force",
            "$contract = Get-Content -Raw -LiteralPath $env:DSR_RELEASE_STATE_CONTRACT | ConvertFrom-Json -AsHashtable",
            "$state = Assert-ReleaseSourceState -RepositoryRoot $env:DSR_RELEASE_STATE_REPOSITORY -RequiredSubmodules $contract",
            "$state | ConvertTo-Json -Compress");
        return await RunProcessAsync(
            "pwsh.exe",
            repositoryRoot,
            new Dictionary<string, string>
            {
                ["DSR_RELEASE_STATE_MODULE"] = Path.Combine(
                    repositoryRoot,
                    "packaging",
                    "ReleaseSourceState.psm1"),
                ["DSR_RELEASE_STATE_CONTRACT"] = fixture.ContractPath,
                ["DSR_RELEASE_STATE_REPOSITORY"] = fixture.RepositoryRoot
            },
            "-NoProfile",
            "-Command",
            command);
    }

    private static async Task<string> GitAsync(string workingDirectory, params string[] arguments)
    {
        var result = await RunProcessAsync("git.exe", workingDirectory, environment: null, arguments);
        Assert.True(
            result.ExitCode == 0,
            $"git {string.Join(' ', arguments)} failed with exit code {result.ExitCode}.\n{result.Output}");
        return result.Output;
    }

    private static async Task<(int ExitCode, string Output)> RunProcessAsync(
        string fileName,
        string workingDirectory,
        IReadOnlyDictionary<string, string>? environment,
        params string[] arguments)
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }
        if (environment is not null)
        {
            foreach (var (name, value) in environment)
            {
                startInfo.Environment[name] = value;
            }
        }

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Unable to start {fileName}.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, $"{await outputTask}\n{await errorTask}");
    }

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
        if (Directory.Exists(_testRoot))
        {
            foreach (var path in Directory.EnumerateFiles(_testRoot, "*", SearchOption.AllDirectories))
            {
                File.SetAttributes(path, FileAttributes.Normal);
            }
            Directory.Delete(_testRoot, recursive: true);
        }
    }

    private sealed record Fixture(
        string RepositoryRoot,
        string ContractPath,
        string MainRevision,
        string SubmoduleRevision);
}
