using System.Diagnostics;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Foundation.Tests.Runtime;

public sealed class ModFolderDiscoveryTests
{
    [Fact]
    public void Discover_ReturnsTopLevelFolderNamesSortedOrdinalIgnoreCase()
    {
        using var fixture = ModFolderFixture.Create();
        fixture.AddDirectory("zeta");
        fixture.AddDirectory("Alpha");
        fixture.AddDirectory("middle");
        File.WriteAllText(Path.Combine(fixture.ModsRoot, "not-a-folder.txt"), "ignored");

        var names = new ModFolderDiscovery().Discover(fixture.RuntimeRoot);

        Assert.Equal(new[] { "Alpha", "middle", "zeta" }, names);
    }

    [Fact]
    public void Discover_ReflectsFolderDeletionWithoutPersistedState()
    {
        using var fixture = ModFolderFixture.Create();
        fixture.AddDirectory("EnemyRandomizer");
        var discovery = new ModFolderDiscovery();

        Assert.Equal(new[] { "EnemyRandomizer" }, discovery.Discover(fixture.RuntimeRoot));
        Directory.Delete(Path.Combine(fixture.ModsRoot, "EnemyRandomizer"));

        Assert.Empty(discovery.Discover(fixture.RuntimeRoot));
    }

    [Fact]
    public void Discover_RejectsReparseDirectory()
    {
        using var fixture = ModFolderFixture.Create();
        var target = Path.Combine(fixture.Container, "target");
        Directory.CreateDirectory(target);
        var link = Path.Combine(fixture.ModsRoot, "linked");
        CreateReparseDirectory(link, target);
        try
        {
            var exception = Assert.Throws<IOException>(() => new ModFolderDiscovery().Discover(fixture.RuntimeRoot));

            Assert.Contains("reparse", exception.Message, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            DeleteReparseDirectory(link);
        }
    }

    private sealed class ModFolderFixture : IDisposable
    {
        private ModFolderFixture(string container)
        {
            Container = container;
            RuntimeRoot = Path.Combine(container, "runtime");
            ModsRoot = Path.Combine(RuntimeRoot, "Mods");
            Directory.CreateDirectory(ModsRoot);
        }

        public string Container { get; }

        public string RuntimeRoot { get; }

        public string ModsRoot { get; }

        public static ModFolderFixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-mod-folders-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new ModFolderFixture(container);
        }

        public void AddDirectory(string name) => Directory.CreateDirectory(Path.Combine(ModsRoot, name));

        public void Dispose() => Directory.Delete(Container, recursive: true);
    }

    private static void CreateReparseDirectory(string path, string target)
    {
        RunCmd($"mklink /J \"{path}\" \"{target}\"");
    }

    private static void DeleteReparseDirectory(string path) => RunCmd($"rmdir \"{path}\"");

    private static void RunCmd(string command)
    {
        using var process = Process.Start(new ProcessStartInfo("cmd.exe", $"/d /c {command}")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            UseShellExecute = false
        }) ?? throw new IOException($"Unable to start cmd.exe for: {command}");
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new IOException($"Command failed ({process.ExitCode}): {error}");
        }
    }
}
