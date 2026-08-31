using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests.Services;

public sealed class RandomizerRuntimeIntegrationTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        $"dsr-randomizer-runtime-integration-{Guid.NewGuid():N}");

    [Fact]
    public void Resolve_FindsTheSingleSelfContainedRandomizerInstallation()
    {
        var runtime = CreateRuntime();

        var tools = RandomizerRuntimeIntegration.Resolve(runtime);

        Assert.Equal(Path.Combine(runtime, "DarkSoulsRemastered.exe"), tools.GameExecutable);
        Assert.Equal(Path.Combine(tools.RandomizerRoot, "DarkSoulsItemRandomizer.exe"), tools.ItemExecutable);
        Assert.Equal(Path.Combine(tools.RandomizerRoot, "DS1EnemyRandomizer.exe"), tools.EnemyExecutable);
        Assert.Equal(Path.Combine(tools.RandomizerRoot, "config_randomizer.toml"), tools.ModEngineConfiguration);
        Assert.Equal(
            Path.Combine(tools.RandomizerRoot, "dist1", "ModEngine", "modengine2_launcher.exe"),
            tools.ModEngineLauncher);
    }

    [Fact]
    public void CreateToolRequest_TargetsTheCopiedRuntimeForItemsAndSelfContainedRootForEnemies()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());

        var item = RandomizerRuntimeIntegration.CreateToolRequest(
            tools,
            RandomizerToolKind.Item);
        var enemy = RandomizerRuntimeIntegration.CreateToolRequest(
            tools,
            RandomizerToolKind.Enemy);

        Assert.Equal(tools.ItemExecutable, item.FileName);
        Assert.Equal(tools.RuntimeRoot, item.WorkingDirectory);
        Assert.Empty(item.Arguments);
        Assert.Equal(tools.EnemyExecutable, enemy.FileName);
        Assert.Equal(tools.RandomizerRoot, enemy.WorkingDirectory);
        Assert.Empty(enemy.Arguments);
    }

    [Fact]
    public void CreateModEngineRequest_UsesAOneLevelDeepGamePathAnchorForDsr()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var configurationPath = Path.Combine(_root, "bridge-only.toml");

        var request = RandomizerRuntimeIntegration.CreateModEngineRequest(
            tools,
            configurationPath);

        Assert.Equal(tools.ModEngineLauncher, request.FileName);
        Assert.Equal(Path.GetDirectoryName(tools.ModEngineLauncher), request.WorkingDirectory);
        Assert.Equal(
            [
                "--launch-target",
                "dsr",
                "--game-path",
                Path.Combine(tools.RuntimeRoot, "chr", "c0000.chrbnd.dcx"),
                "--config",
                configurationPath
            ],
            request.Arguments);
    }

    [Fact]
    public void HasRequiredModEngineConfiguration_AcceptsExactActiveBridgeAndHeapPatchPaths()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());

        Assert.True(RandomizerRuntimeIntegration.HasRequiredModEngineConfiguration(
            tools,
            _root,
            File.ReadAllBytes(tools.ModEngineConfiguration)));
    }

    [Fact]
    public void HasRequiredModEngineConfiguration_RejectsCommentsWrongTableAndEmptyArrays()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var validEntries = ValidExternalDllEntries(tools);
        var invalidConfigurations = new[]
        {
            $"[modengine]\n# external_dlls = [{validEntries}]\nexternal_dlls = []\n",
            $"[other]\nexternal_dlls = [{validEntries}]\n[modengine]\nexternal_dlls = []\n",
            "[modengine]\nexternal_dlls = []\n"
        };

        foreach (var configuration in invalidConfigurations)
        {
            File.WriteAllText(tools.ModEngineConfiguration, configuration);
            Assert.False(RandomizerRuntimeIntegration.HasRequiredModEngineConfiguration(
                tools,
                _root,
                File.ReadAllBytes(tools.ModEngineConfiguration)));
        }
    }

    [Fact]
    public void HasRequiredModEngineConfiguration_RejectsItemExecutableAsAnExternalDll()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        File.WriteAllText(
            tools.ModEngineConfiguration,
            $"[modengine]\nexternal_dlls = [{ValidExternalDllEntries(tools)}, {TomlString(tools.ItemExecutable)}]\n");

        Assert.False(RandomizerRuntimeIntegration.HasRequiredModEngineConfiguration(
            tools,
            _root,
            File.ReadAllBytes(tools.ModEngineConfiguration)));
    }

    [Fact]
    public void TryCreateBridgedModEngineConfiguration_DoesNotTreatCommentedBridgeModAsActive()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var (_, heapPatch) = RandomizerRuntimeIntegration.GetRequiredModEngineDllPaths(tools, _root);
        var bridgeRoot = Path.Combine(_root, "components", "rmm-bridge");
        var source =
            "[modengine]\n" +
            $"external_dlls = [{TomlString(heapPatch)}]\n\n" +
            "[extension.mod_loader]\n" +
            "enabled = true\n" +
            "mods = [\n" +
            $"    # old bridge: {{ path = {TomlString(bridgeRoot)} }}\n" +
            $"    {{ enabled = true, name = \"randomizer\", path = {TomlString(tools.RandomizerRoot)} }},\n" +
            "]\n";

        var created = RandomizerRuntimeIntegration.TryCreateBridgedModEngineConfiguration(
            tools,
            _root,
            System.Text.Encoding.UTF8.GetBytes(source),
            out var configurationBytes);

        Assert.True(created);
        var configuration = System.Text.Encoding.UTF8.GetString(configurationBytes);
        Assert.Equal(1, configuration.Split(TomlString(bridgeRoot), StringSplitOptions.None).Length - 1);
        Assert.Contains(
            $"{{ enabled = true, name = \"mod1\", path = {TomlString(bridgeRoot)} }}",
            configuration,
            StringComparison.Ordinal);
    }

    [Fact]
    public void TryCreateBridgedModEngineConfiguration_ReplacesStaleAndDisabledBridgeEntriesExactlyOnce()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var (bridge, heapPatch) = RandomizerRuntimeIntegration.GetRequiredModEngineDllPaths(tools, _root);
        var bridgeRoot = Path.GetDirectoryName(bridge)!;
        var staleBridge = Path.Combine(_root, "old-components", "rmm-bridge", "DSRRandomizer.RmmBridge.dll");
        var staleBridgeRoot = Path.GetDirectoryName(staleBridge)!;
        var extraMod = Path.Combine(_root, "mods", "extra");
        var source =
            "[modengine]\n" +
            $"external_dlls = [{TomlString(staleBridge)}, {TomlString(bridge)}, {TomlString(heapPatch)}]\n\n" +
            "[extension.mod_loader]\n" +
            "enabled = true\n" +
            "loose_params = false\n" +
            "mods = [\n" +
            $"    {{ enabled = true, name = \"randomizer\", path = {TomlString(tools.RandomizerRoot)} }},\n" +
            $"    {{ enabled = true, name = \"mod1\", path = {TomlString(staleBridgeRoot)} }},\n" +
            $"    {{ enabled = false, name = \"mod1\", path = {TomlString(bridgeRoot)} }},\n" +
            $"    {{ enabled = true, name = \"extra\", path = {TomlString(extraMod)} }},\n" +
            "    # trailing note from a previous generator\n" +
            "]\n";

        var created = RandomizerRuntimeIntegration.TryCreateBridgedModEngineConfiguration(
            tools,
            _root,
            System.Text.Encoding.UTF8.GetBytes(source),
            out var configurationBytes);

        Assert.True(created);
        var configuration = System.Text.Encoding.UTF8.GetString(configurationBytes);
        Assert.Equal(1, configuration.Split(TomlString(bridge), StringSplitOptions.None).Length - 1);
        Assert.Equal(1, configuration.Split(TomlString(heapPatch), StringSplitOptions.None).Length - 1);
        Assert.Equal(1, configuration.Split(TomlString(bridgeRoot), StringSplitOptions.None).Length - 1);
        Assert.DoesNotContain(staleBridgeRoot, configuration, StringComparison.OrdinalIgnoreCase);
        Assert.Contains(TomlString(extraMod), configuration, StringComparison.Ordinal);
        Assert.Contains(
            $"{{ enabled = true, name = \"mod1\", path = {TomlString(bridgeRoot)} }}",
            configuration,
            StringComparison.Ordinal);
    }

    [Fact]
    public void TryCreateBridgedModEngineConfiguration_RejectsDuplicateModFields()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var (_, heapPatch) = RandomizerRuntimeIntegration.GetRequiredModEngineDllPaths(tools, _root);
        var source =
            "[modengine]\n" +
            $"external_dlls = [{TomlString(heapPatch)}]\n\n" +
            "[extension.mod_loader]\n" +
            "enabled = true\n" +
            "mods = [\n" +
            $"    {{ enabled = true, enabled = false, name = \"randomizer\", path = {TomlString(tools.RandomizerRoot)} }},\n" +
            "]\n";

        var created = RandomizerRuntimeIntegration.TryCreateBridgedModEngineConfiguration(
            tools,
            _root,
            System.Text.Encoding.UTF8.GetBytes(source),
            out _);

        Assert.False(created);
    }

    [Fact]
    public void TryCreateBridgedModEngineConfiguration_RejectsDisabledModLoader()
    {
        var tools = RandomizerRuntimeIntegration.Resolve(CreateRuntime());
        var (_, heapPatch) = RandomizerRuntimeIntegration.GetRequiredModEngineDllPaths(tools, _root);
        var source =
            "[modengine]\n" +
            $"external_dlls = [{TomlString(heapPatch)}]\n\n" +
            "[extension.mod_loader]\n" +
            "enabled = false\n" +
            $"mods = [{{ enabled = true, name = \"randomizer\", path = {TomlString(tools.RandomizerRoot)} }}]\n";

        Assert.False(RandomizerRuntimeIntegration.TryCreateBridgedModEngineConfiguration(
            tools,
            _root,
            System.Text.Encoding.UTF8.GetBytes(source),
            out _));
    }

    [Fact]
    public void Resolve_WhenBundledModEngineDllIsMissing_FailsPreflight()
    {
        var runtime = CreateRuntime();
        var modEngineDll = Directory.EnumerateFiles(
            runtime,
            "modengine2.dll",
            SearchOption.AllDirectories).Single();
        File.Delete(modEngineDll);

        Assert.Throws<FileNotFoundException>(() => RandomizerRuntimeIntegration.Resolve(runtime));
    }

    private string CreateRuntime()
    {
        var runtime = Path.Combine(_root, "runtimes", "runtime-current");
        var randomizer = Path.Combine(runtime, "Mods", "Enemy-package", "DS1EnemyRandomizer");
        Directory.CreateDirectory(Path.Combine(randomizer, "dist1", "ModEngine"));
        Directory.CreateDirectory(Path.Combine(randomizer, "dist1", "DLL"));
        Directory.CreateDirectory(Path.Combine(runtime, "chr"));
        File.WriteAllText(Path.Combine(runtime, "chr", "c0000.chrbnd.dcx"), "anchor");
        var bridge = Path.Combine(
            _root,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridge.dll");
        Directory.CreateDirectory(Path.GetDirectoryName(bridge)!);
        File.WriteAllText(Path.Combine(runtime, "DarkSoulsRemastered.exe"), "game");
        File.WriteAllText(Path.Combine(randomizer, "DarkSoulsItemRandomizer.exe"), "item");
        File.WriteAllText(Path.Combine(randomizer, "DS1EnemyRandomizer.exe"), "enemy");
        File.WriteAllText(bridge, "bridge");
        var heapPatch = Path.Combine(randomizer, "dist1", "DLL", "DS1HeapPatch.dll");
        File.WriteAllText(heapPatch, "heap");
        File.WriteAllText(
            Path.Combine(randomizer, "config_randomizer.toml"),
            $"[modengine]\nexternal_dlls = [{TomlString(bridge)}, {TomlString(heapPatch)}]\n");
        File.WriteAllText(
            Path.Combine(randomizer, "dist1", "ModEngine", "modengine2_launcher.exe"),
            "modengine");
        Directory.CreateDirectory(Path.Combine(randomizer, "dist1", "ModEngine", "modengine2", "bin"));
        File.WriteAllText(
            Path.Combine(randomizer, "dist1", "ModEngine", "modengine2", "bin", "modengine2.dll"),
            "modengine-dll");
        return runtime;
    }

    private string ValidExternalDllEntries(RandomizerRuntimeTools tools) => string.Join(
        ", ",
        TomlString(Path.Combine(
            _root,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridge.dll")),
        TomlString(Path.Combine(tools.RandomizerRoot, "dist1", "DLL", "DS1HeapPatch.dll")));

    private static string TomlString(string value) =>
        $"\"{value.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal)}\"";

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }
}
