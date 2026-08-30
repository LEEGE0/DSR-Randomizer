using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.RmmBridgeHost.GameParam;

namespace DSRRandomizer.RmmBridgeHost.Tests.GameParam;

public sealed class GameParamSourceResolverTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(), $"dsr-gameparam-resolver-{Guid.NewGuid():N}");
    private readonly List<string> _junctions = [];

    [Fact]
    public void Resolve_OneNestedEnemyRandomizer_ReturnsContainedCanonicalInputsAndOutputs()
    {
        Fixture fixture = CreateFixture();

        GameParamSourceSet result = Resolver().Resolve(
            fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot);

        Assert.Equal(Path.GetFullPath(fixture.EnemyRoot), result.EnemyRandomizerRoot);
        Assert.Equal(Path.Combine(fixture.EnemyRoot, @"dist1\Vanilla\GameParam.parambnd.dcx"), result.BasePath);
        Assert.Equal(Path.Combine(fixture.EnemyRoot, @"param\GameParam\GameParam.parambnd.dcx"), result.RandomizedPath);
        Assert.Equal(Path.Combine(fixture.SteamRoot, @"overhaul\GameParam.parambnd.dcx"), result.OverhaulPath);
        Assert.Equal(2, result.DefinitionPaths.Count);
        Assert.Equal(
            Path.Combine(fixture.ExternalRoot, @"components\rmm-bridge\content\overhaul\GameParam.parambnd.dcx"),
            result.OutputPath);
        Assert.Equal(
            Path.Combine(fixture.ExternalRoot, @"components\rmm-bridge\content\overhaul\gameparam-merge-manifest.json"),
            result.ManifestPath);
    }

    [Fact]
    public void Resolve_NoEnemyRandomizer_FailsClosed()
    {
        Fixture fixture = CreateFixture(createEnemy: false);

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));

        Assert.Contains("exactly one", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Resolve_TwoEnemyRandomizers_FailsClosed()
    {
        Fixture fixture = CreateFixture();
        CreateEnemy(Path.Combine(fixture.ModsRoot, "second-package", "DS1EnemyRandomizer"));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));

        Assert.Contains("exactly one", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Resolve_TraversalRuntimeId_FailsBeforeDiscovery()
    {
        Fixture fixture = CreateFixture();

        Assert.Throws<ArgumentException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, @"..\outside", fixture.SteamRoot));
    }

    [Theory]
    [InlineData(@"dist1\Vanilla\GameParam.parambnd.dcx")]
    [InlineData(@"param\GameParam\GameParam.parambnd.dcx")]
    [InlineData(@"dist1\Defs")]
    public void Resolve_MissingRandomizerInput_FailsClosed(string relativePath)
    {
        Fixture fixture = CreateFixture();
        string path = Path.Combine(fixture.EnemyRoot, relativePath);
        if (Directory.Exists(path))
            Directory.Delete(path, recursive: true);
        else
            File.Delete(path);

        Assert.Throws<InvalidDataException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));
    }

    [Fact]
    public void Resolve_MissingSteamOverhaul_FailsClosed()
    {
        Fixture fixture = CreateFixture();
        File.Delete(Path.Combine(fixture.SteamRoot, @"overhaul\GameParam.parambnd.dcx"));

        Assert.Throws<InvalidDataException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));
    }

    [Fact]
    public void Resolve_HardLinkedInput_FailsClosed()
    {
        Fixture fixture = CreateFixture();
        string basePath = Path.Combine(fixture.EnemyRoot, @"dist1\Vanilla\GameParam.parambnd.dcx");
        string aliasPath = Path.Combine(fixture.EnemyRoot, "base-alias.dcx");
        if (!CreateHardLink(aliasPath, basePath, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());

        IOException exception = Assert.Throws<IOException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));

        Assert.Contains("link", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Resolve_ReparseInputDirectory_FailsClosed()
    {
        Fixture fixture = CreateFixture(createEnemy: false);
        string target = Path.Combine(_root, "outside-enemy");
        CreateEnemy(target);
        string package = Path.Combine(fixture.ModsRoot, "package");
        Directory.CreateDirectory(package);
        string junction = Path.Combine(package, "DS1EnemyRandomizer");
        CreateJunction(junction, target);

        Assert.Throws<IOException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));
    }

    [Fact]
    public void Resolve_ReparseOutputEscape_FailsClosed()
    {
        Fixture fixture = CreateFixture();
        string content = Path.Combine(fixture.ExternalRoot, @"components\rmm-bridge\content");
        Directory.CreateDirectory(Path.GetDirectoryName(content)!);
        string outside = Path.Combine(_root, "outside-output");
        Directory.CreateDirectory(outside);
        CreateJunction(content, outside);

        Assert.Throws<UnauthorizedAccessException>(() =>
            Resolver().Resolve(fixture.ExternalRoot, fixture.RuntimeId, fixture.SteamRoot));
    }

    public void Dispose()
    {
        foreach (string junction in _junctions.OrderByDescending(static path => path.Length))
        {
            if (Directory.Exists(junction))
                Directory.Delete(junction);
        }
        if (Directory.Exists(_root))
            Directory.Delete(_root, recursive: true);
    }

    private GameParamSourceResolver Resolver() => new(new WindowsPathCanonicalizer());

    private Fixture CreateFixture(bool createEnemy = true)
    {
        string external = Path.Combine(_root, "external");
        const string runtimeId = "runtime-a39cb5e0";
        string mods = Path.Combine(external, "runtimes", runtimeId, "Mods");
        string enemy = Path.Combine(mods, "package", "DS1EnemyRandomizer");
        string steam = Path.Combine(_root, "steam");
        Directory.CreateDirectory(mods);
        Directory.CreateDirectory(Path.Combine(steam, "overhaul"));
        File.WriteAllBytes(Path.Combine(steam, @"overhaul\GameParam.parambnd.dcx"), [7]);
        if (createEnemy)
            CreateEnemy(enemy);
        return new Fixture(external, runtimeId, mods, enemy, steam);
    }

    private static void CreateEnemy(string root)
    {
        Directory.CreateDirectory(Path.Combine(root, @"dist1\Vanilla"));
        Directory.CreateDirectory(Path.Combine(root, @"param\GameParam"));
        Directory.CreateDirectory(Path.Combine(root, @"dist1\Defs"));
        File.WriteAllBytes(Path.Combine(root, @"dist1\Vanilla\GameParam.parambnd.dcx"), [1]);
        File.WriteAllBytes(Path.Combine(root, @"param\GameParam\GameParam.parambnd.dcx"), [2]);
        File.WriteAllText(Path.Combine(root, @"dist1\Defs\B.xml"), "b");
        File.WriteAllText(Path.Combine(root, @"dist1\Defs\A.xml"), "a");
    }

    private void CreateJunction(string junction, string target)
    {
        var start = new ProcessStartInfo("cmd.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
        };
        start.ArgumentList.Add("/c");
        start.ArgumentList.Add("mklink");
        start.ArgumentList.Add("/J");
        start.ArgumentList.Add(junction);
        start.ArgumentList.Add(target);
        using Process process = Process.Start(start)!;
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(process.StandardError.ReadToEnd());
        _junctions.Add(junction);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLink(
        string fileName,
        string existingFileName,
        IntPtr securityAttributes);

    private sealed record Fixture(
        string ExternalRoot,
        string RuntimeId,
        string ModsRoot,
        string EnemyRoot,
        string SteamRoot);
}
