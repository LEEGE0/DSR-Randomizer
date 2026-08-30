using System.Security.Cryptography;
using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.RmmBridgeHost.GameParam;
using SoulsFormats;
using Xunit.Abstractions;

namespace DSRRandomizer.RmmBridgeHost.Tests.GameParam;

public sealed class GameParamPublisherTests(ITestOutputHelper output) : IDisposable
{
    private const int ParamFileId = 7;
    private readonly string _root = Path.Combine(
        Path.GetTempPath(), $"dsr-gameparam-publisher-{Guid.NewGuid():N}");

    [Fact]
    public async Task PublishAsync_MergesToVerifiedOutputAndWritesStrictManifest()
    {
        Fixture fixture = CreateFixture(randomValue: 20);
        var events = new List<string>();
        var publisher = new GameParamPublisher(
            new GameParamThreeWayMerger(), fixture.Sources, events.Add);

        await publisher.PublishAsync(CancellationToken.None);

        Assert.Equal(20, ReadValue(fixture.Sources.OutputPath, 1, fixture.Definition));
        Assert.Equal(200, ReadValue(fixture.Sources.OutputPath, 2, fixture.Definition));
        Assert.Equal(300, ReadValue(fixture.Sources.OutputPath, 3, fixture.Definition));
        using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(fixture.Sources.ManifestPath));
        Assert.Equal(1, manifest.RootElement.GetProperty("schemaVersion").GetInt32());
        Assert.Equal(GameParamPublisher.DependencyCommit,
            manifest.RootElement.GetProperty("dependencyCommit").GetString());
        Assert.Equal(Sha256(fixture.Sources.OutputPath),
            manifest.RootElement.GetProperty("outputSha256").GetString());
        Assert.Equal(4, manifest.RootElement.GetProperty("sources").GetArrayLength());
        Assert.Contains(events, entry => entry.Contains("cache=miss", StringComparison.Ordinal));
        Assert.Empty(Directory.EnumerateFiles(fixture.Sources.OutputDirectory, "*.staging-*"));
    }

    [Fact]
    public async Task PublishAsync_UnchangedSourcesAndVerifiedOutput_UsesCache()
    {
        Fixture fixture = CreateFixture(randomValue: 20);
        var events = new List<string>();
        var publisher = new GameParamPublisher(
            new GameParamThreeWayMerger(), fixture.Sources, events.Add);
        await publisher.PublishAsync(CancellationToken.None);
        DateTime initialWrite = File.GetLastWriteTimeUtc(fixture.Sources.OutputPath);

        await publisher.PublishAsync(CancellationToken.None);

        Assert.Equal(initialWrite, File.GetLastWriteTimeUtc(fixture.Sources.OutputPath));
        Assert.Contains(events, entry => entry.Contains("cache=hit", StringComparison.Ordinal));
    }

    [Fact]
    public async Task PublishAsync_RandomizedSourceChanges_RebuildsOutputAndManifest()
    {
        Fixture fixture = CreateFixture(randomValue: 20);
        var events = new List<string>();
        var publisher = new GameParamPublisher(
            new GameParamThreeWayMerger(), fixture.Sources, events.Add);
        await publisher.PublishAsync(CancellationToken.None);
        string firstHash = Sha256(fixture.Sources.OutputPath);
        WriteBinder(fixture.Sources.RandomizedPath, fixture.Definition, (1, 30), (2, 10));

        await publisher.PublishAsync(CancellationToken.None);

        Assert.Equal(30, ReadValue(fixture.Sources.OutputPath, 1, fixture.Definition));
        Assert.NotEqual(firstHash, Sha256(fixture.Sources.OutputPath));
        Assert.Equal(2, events.Count(entry => entry.Contains("cache=miss", StringComparison.Ordinal)));
    }

    [Fact]
    public async Task PublishAsync_CorruptCachedOutput_RebuildsInsteadOfReusing()
    {
        Fixture fixture = CreateFixture(randomValue: 20);
        var events = new List<string>();
        var publisher = new GameParamPublisher(
            new GameParamThreeWayMerger(), fixture.Sources, events.Add);
        await publisher.PublishAsync(CancellationToken.None);
        File.WriteAllBytes(fixture.Sources.OutputPath, [1, 2, 3]);

        await publisher.PublishAsync(CancellationToken.None);

        Assert.Equal(20, ReadValue(fixture.Sources.OutputPath, 1, fixture.Definition));
        Assert.Equal(2, events.Count(entry => entry.Contains("cache=miss", StringComparison.Ordinal)));
    }

    [Fact]
    public async Task PublishAsync_MergeFailure_CleansOnlyItsOwnStagingAndPreservesExistingOutput()
    {
        Fixture fixture = CreateFixture(randomValue: 20);
        var publisher = new GameParamPublisher(
            new GameParamThreeWayMerger(), fixture.Sources, _ => { });
        await publisher.PublishAsync(CancellationToken.None);
        byte[] original = File.ReadAllBytes(fixture.Sources.OutputPath);
        string unrelated = Path.Combine(fixture.Sources.OutputDirectory, "keep.staging-unrelated");
        File.WriteAllText(unrelated, "keep");
        File.WriteAllBytes(fixture.Sources.RandomizedPath, [1, 2, 3]);

        await Assert.ThrowsAnyAsync<Exception>(() => publisher.PublishAsync(CancellationToken.None));

        Assert.Equal(original, File.ReadAllBytes(fixture.Sources.OutputPath));
        Assert.True(File.Exists(unrelated));
        Assert.Single(Directory.EnumerateFiles(fixture.Sources.OutputDirectory, "*.staging-*"));
    }

    [Fact]
    public async Task RealInputs_PublishesEightChangedParamsAndPreservesTwentyOverhaulOnlyRows()
    {
        if (!"1".Equals(
                Environment.GetEnvironmentVariable("DSR_GAMEPARAM_REAL_INTEGRATION"),
                StringComparison.Ordinal))
            return;

        string externalRoot = Environment.GetEnvironmentVariable("DSR_GAMEPARAM_EXTERNAL_ROOT")
            ?? throw new InvalidOperationException("DSR_GAMEPARAM_EXTERNAL_ROOT is required.");
        string runtimeId = Environment.GetEnvironmentVariable("DSR_GAMEPARAM_RUNTIME_ID")
            ?? throw new InvalidOperationException("DSR_GAMEPARAM_RUNTIME_ID is required.");
        string steamRoot = Environment.GetEnvironmentVariable("DSR_GAMEPARAM_STEAM_ROOT")
            ?? throw new InvalidOperationException("DSR_GAMEPARAM_STEAM_ROOT is required.");
        string rmmPath = Path.GetFullPath(Environment.GetEnvironmentVariable("DSR_GAMEPARAM_RMM_PATH")
            ?? throw new InvalidOperationException("DSR_GAMEPARAM_RMM_PATH is required."));
        Assert.True(File.Exists(rmmPath));
        string stagingRoot = Path.GetFullPath(Environment.GetEnvironmentVariable("DSR_GAMEPARAM_STAGING_ROOT")
            ?? throw new InvalidOperationException("DSR_GAMEPARAM_STAGING_ROOT is required."));
        string requiredParent = Path.GetFullPath(Path.Combine(externalRoot, "staging"));
        Assert.Equal(requiredParent, Path.GetDirectoryName(stagingRoot), ignoreCase: true);
        Assert.StartsWith("gameparam-task2-", Path.GetFileName(stagingRoot), StringComparison.Ordinal);
        Assert.False(Directory.Exists(stagingRoot));

        var resolver = new GameParamSourceResolver(new WindowsPathCanonicalizer());
        GameParamSourceSet resolved = resolver.Resolve(externalRoot, runtimeId, steamRoot);
        var sources = resolved with
        {
            OutputDirectory = stagingRoot,
            OutputPath = Path.Combine(stagingRoot, "GameParam.parambnd.dcx"),
            ManifestPath = Path.Combine(stagingRoot, "gameparam-merge-manifest.json"),
        };
        string[] inputs =
            [sources.BasePath, sources.RandomizedPath, sources.OverhaulPath, .. sources.DefinitionPaths, rmmPath];
        Dictionary<string, string> before = inputs.ToDictionary(
            static path => path,
            Sha256,
            StringComparer.OrdinalIgnoreCase);

        try
        {
            var events = new List<string>();
            await new GameParamPublisher(new GameParamThreeWayMerger(), sources, events.Add)
                .PublishAsync(CancellationToken.None);

            PARAMDEF[] definitions = sources.DefinitionPaths
                .Select(path => PARAMDEF.XmlDeserialize(path))
                .ToArray();
            BND3 baseBnd = BND3.Read(sources.BasePath);
            BND3 randomBnd = BND3.Read(sources.RandomizedPath);
            BND3 targetBnd = BND3.Read(sources.OverhaulPath);
            BND3 outputBnd = BND3.Read(sources.OutputPath);
            Dictionary<int, BinderFile> baseFiles = baseBnd.Files.ToDictionary(static file => file.ID);
            Dictionary<int, BinderFile> randomFiles = randomBnd.Files.ToDictionary(static file => file.ID);
            Dictionary<int, BinderFile> targetFiles = targetBnd.Files.ToDictionary(static file => file.ID);
            Dictionary<int, BinderFile> outputFiles = outputBnd.Files.ToDictionary(static file => file.ID);
            int[] byteChangedEntries = baseFiles.Keys.Union(randomFiles.Keys)
                .Where(id => !baseFiles.TryGetValue(id, out BinderFile? baseFile)
                    || !randomFiles.TryGetValue(id, out BinderFile? randomFile)
                    || !baseFile.Bytes.AsSpan().SequenceEqual(randomFile.Bytes))
                .Order()
                .ToArray();
            int[] changedEntries = byteChangedEntries
                .Where(id => HasFunctionalParamChange(baseFiles[id], randomFiles[id], definitions))
                .ToArray();
            Assert.Equal(8, changedEntries.Length);

            foreach (int id in changedEntries)
            {
                BinderFile baseFile = baseFiles[id];
                BinderFile randomFile = randomFiles[id];
                BinderFile targetFile = targetFiles[id];
                BinderFile outputFile = outputFiles[id];
                (PARAM Base, PARAM Random, PARAM Target, PARAM Output) parsed =
                    ReadCompatibleParams(baseFile, randomFile, targetFile, outputFile, definitions);
                Dictionary<int, PARAM.Row> baseRows = parsed.Base.Rows.ToDictionary(static row => row.ID);
                Dictionary<int, PARAM.Row> randomRows = parsed.Random.Rows.ToDictionary(static row => row.ID);
                Dictionary<int, PARAM.Row> targetRows = parsed.Target.Rows.ToDictionary(static row => row.ID);
                Dictionary<int, PARAM.Row> outputRows = parsed.Output.Rows.ToDictionary(static row => row.ID);

                foreach ((int rowId, PARAM.Row randomRow) in randomRows)
                {
                    if (!baseRows.TryGetValue(rowId, out PARAM.Row? baseRow)
                        || !RowsEqual(baseRow, randomRow))
                        Assert.True(RowsEqual(randomRow, outputRows[rowId]));
                }
                foreach (int deletedId in baseRows.Keys.Except(randomRows.Keys))
                    Assert.DoesNotContain(deletedId, outputRows.Keys);
            }

            int overhaulOnlyRows = 0;
            foreach (int id in baseFiles.Keys.Intersect(randomFiles.Keys).Intersect(targetFiles.Keys))
            {
                BinderFile baseFile = baseFiles[id];
                BinderFile randomFile = randomFiles[id];
                BinderFile targetFile = targetFiles[id];
                if (!baseFile.Name.EndsWith(".param", StringComparison.OrdinalIgnoreCase)
                    || !randomFile.Name.EndsWith(".param", StringComparison.OrdinalIgnoreCase)
                    || !targetFile.Name.EndsWith(".param", StringComparison.OrdinalIgnoreCase))
                    continue;
                PARAM baseParam = PARAM.Read(baseFile.Bytes);
                PARAM randomParam = PARAM.Read(randomFile.Bytes);
                PARAM targetParam = PARAM.Read(targetFile.Bytes);
                HashSet<int> baseIds = baseParam.Rows.Select(static row => row.ID).ToHashSet();
                HashSet<int> randomIds = randomParam.Rows.Select(static row => row.ID).ToHashSet();
                int[] targetOnlyIds = targetParam.Rows
                    .Select(static row => row.ID)
                    .Where(rowId => !baseIds.Contains(rowId) && !randomIds.Contains(rowId))
                    .ToArray();
                overhaulOnlyRows += targetOnlyIds.Length;

                if (!changedEntries.Contains(id))
                {
                    Assert.Equal(targetFile.Bytes, outputFiles[id].Bytes);
                    continue;
                }

                (PARAM Base, PARAM Random, PARAM Target, PARAM Output) parsed = ReadCompatibleParams(
                    baseFile, randomFile, targetFile, outputFiles[id], definitions);
                Dictionary<int, PARAM.Row> targetRows = parsed.Target.Rows.ToDictionary(static row => row.ID);
                Dictionary<int, PARAM.Row> outputRows = parsed.Output.Rows.ToDictionary(static row => row.ID);
                foreach (int rowId in targetOnlyIds)
                    Assert.True(RowsEqual(targetRows[rowId], outputRows[rowId]));
            }
            Assert.Equal(20, overhaulOnlyRows);

            using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(sources.ManifestPath));
            output.WriteLine(
                $"changedParams={changedEntries.Length}; overhaulOnlyRows={overhaulOnlyRows}; " +
                $"changedEntries={string.Join(',', changedEntries)}; outputSha256={Sha256(sources.OutputPath)}; " +
                $"baseSha256={before[sources.BasePath]}; randomizedSha256={before[sources.RandomizedPath]}; " +
                $"overhaulSha256={before[sources.OverhaulPath]}; defs={sources.DefinitionPaths.Count}; " +
                $"rmmSha256={before[rmmPath]}; rmmLength={new FileInfo(rmmPath).Length}; " +
                $"manifestOutputSha256={manifest.RootElement.GetProperty("outputSha256").GetString()}");
        }
        finally
        {
            try
            {
                Dictionary<string, string> after = inputs.ToDictionary(
                    static path => path,
                    Sha256,
                    StringComparer.OrdinalIgnoreCase);
                foreach ((string path, string hash) in before)
                    Assert.Equal(hash, after[path]);
                output.WriteLine(
                    $"inputIntegrity=unchanged; baseSha256={after[sources.BasePath]}; " +
                    $"randomizedSha256={after[sources.RandomizedPath]}; " +
                    $"overhaulSha256={after[sources.OverhaulPath]}; " +
                    $"rmmSha256={after[rmmPath]}; rmmLength={new FileInfo(rmmPath).Length}; " +
                    $"defs={sources.DefinitionPaths.Count}");
            }
            finally
            {
                string validated = Path.GetFullPath(stagingRoot);
                if (Path.GetDirectoryName(validated)!.Equals(requiredParent, StringComparison.OrdinalIgnoreCase)
                    && Path.GetFileName(validated).StartsWith("gameparam-task2-", StringComparison.Ordinal)
                    && Directory.Exists(validated))
                {
                    Directory.Delete(validated, recursive: true);
                }
            }
        }
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
            Directory.Delete(_root, recursive: true);
    }

    private Fixture CreateFixture(int randomValue)
    {
        var def = new PARAMDEF { ParamType = "TestParam", DataVersion = 100 };
        def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.s32, "value"));
        string enemy = Path.Combine(_root, "enemy");
        string steam = Path.Combine(_root, "steam");
        string outputDirectory = Path.Combine(_root, "external", "components", "rmm-bridge", "content", "overhaul");
        string defs = Path.Combine(enemy, @"dist1\Defs");
        Directory.CreateDirectory(defs);
        Directory.CreateDirectory(Path.Combine(enemy, @"dist1\Vanilla"));
        Directory.CreateDirectory(Path.Combine(enemy, @"param\GameParam"));
        Directory.CreateDirectory(Path.Combine(steam, "overhaul"));
        string defPath = Path.Combine(defs, "TestParam.xml");
        def.XmlSerialize(defPath);
        string basePath = Path.Combine(enemy, @"dist1\Vanilla\GameParam.parambnd.dcx");
        string randomPath = Path.Combine(enemy, @"param\GameParam\GameParam.parambnd.dcx");
        string overhaulPath = Path.Combine(steam, @"overhaul\GameParam.parambnd.dcx");
        WriteBinder(basePath, def, (1, 10), (2, 10));
        WriteBinder(randomPath, def, (1, randomValue), (2, 10));
        WriteBinder(overhaulPath, def, (1, 100), (2, 200), (3, 300));
        var sources = new GameParamSourceSet(
            Path.Combine(_root, "external"),
            Path.Combine(_root, "external", "runtimes", "runtime-a39cb5e0"),
            enemy,
            basePath,
            randomPath,
            defs,
            [defPath],
            steam,
            overhaulPath,
            outputDirectory,
            Path.Combine(outputDirectory, "GameParam.parambnd.dcx"),
            Path.Combine(outputDirectory, "gameparam-merge-manifest.json"));
        return new Fixture(sources, def);
    }

    private static void WriteBinder(string path, PARAMDEF def, params (int ID, int Value)[] rows)
    {
        var param = new PARAM { ParamType = def.ParamType, ParamdefDataVersion = def.DataVersion, Rows = [] };
        param.ApplyParamdef(def);
        foreach ((int id, int value) in rows)
        {
            var row = new PARAM.Row(id, $"row-{id}", def);
            row.Cells[0].Value = value;
            param.Rows.Add(row);
        }
        var binder = new BND3
        {
            Version = "TEST",
            Compression = new DCX.DcxDfltCompressionInfo(
                DCX.DfltCompressionPreset.DCX_DFLT_10000_24_9),
            Files = [new BinderFile(Binder.FileFlags.Flag1, ParamFileId, "TestParam.param", param.Write())]
        };
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, binder.Write());
    }

    private static int ReadValue(string path, int rowId, PARAMDEF def)
    {
        BND3 binder = BND3.Read(path);
        PARAM param = PARAM.Read(Assert.Single(binder.Files).Bytes);
        param.ApplyParamdef(def);
        return (int)Assert.Single(param.Rows, row => row.ID == rowId).Cells[0].Value;
    }

    private static string Sha256(string path) => Convert.ToHexString(
        SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();

    private static (PARAM Base, PARAM Random, PARAM Target, PARAM Output) ReadCompatibleParams(
        BinderFile baseFile,
        BinderFile randomFile,
        BinderFile targetFile,
        BinderFile outputFile,
        IReadOnlyCollection<PARAMDEF> definitions)
    {
        PARAM baseParam = PARAM.Read(baseFile.Bytes);
        PARAM randomParam = PARAM.Read(randomFile.Bytes);
        PARAM targetParam = PARAM.Read(targetFile.Bytes);
        PARAM outputParam = PARAM.Read(outputFile.Bytes);
        PARAMDEF definition = Assert.Single(definitions, def =>
            string.Equals(def.ParamType, baseParam.ParamType, StringComparison.Ordinal)
            && def.GetRowSize() == baseParam.DetectedSize
            && def.GetRowSize() == randomParam.DetectedSize
            && def.GetRowSize() == targetParam.DetectedSize
            && def.GetRowSize() == outputParam.DetectedSize);
        baseParam.ApplyParamdef(definition);
        randomParam.ApplyParamdef(definition);
        targetParam.ApplyParamdef(definition);
        outputParam.ApplyParamdef(definition);
        return (baseParam, randomParam, targetParam, outputParam);
    }

    private static bool HasFunctionalParamChange(
        BinderFile baseFile,
        BinderFile randomFile,
        IReadOnlyCollection<PARAMDEF> definitions)
    {
        if (!baseFile.Name.EndsWith(".param", StringComparison.OrdinalIgnoreCase)
            || !randomFile.Name.EndsWith(".param", StringComparison.OrdinalIgnoreCase))
            return false;
        PARAM baseParam = PARAM.Read(baseFile.Bytes);
        PARAM randomParam = PARAM.Read(randomFile.Bytes);
        PARAMDEF definition = Assert.Single(definitions, def =>
            string.Equals(def.ParamType, baseParam.ParamType, StringComparison.Ordinal)
            && def.GetRowSize() == baseParam.DetectedSize
            && def.GetRowSize() == randomParam.DetectedSize);
        baseParam.ApplyParamdef(definition);
        randomParam.ApplyParamdef(definition);
        Dictionary<int, PARAM.Row> baseRows = baseParam.Rows.ToDictionary(static row => row.ID);
        Dictionary<int, PARAM.Row> randomRows = randomParam.Rows.ToDictionary(static row => row.ID);
        return baseRows.Count != randomRows.Count
            || baseRows.Any(item =>
                !randomRows.TryGetValue(item.Key, out PARAM.Row? randomRow)
                || !RowsEqual(item.Value, randomRow));
    }

    private static bool RowsEqual(PARAM.Row left, PARAM.Row right)
    {
        if (left.Cells.Count != right.Cells.Count)
            return false;
        for (int index = 0; index < left.Cells.Count; index++)
        {
            object leftValue = left.Cells[index].Value;
            object rightValue = right.Cells[index].Value;
            bool equal = (leftValue, rightValue) switch
            {
                (byte[] leftBytes, byte[] rightBytes) => leftBytes.AsSpan().SequenceEqual(rightBytes),
                (float leftFloat, float rightFloat) =>
                    BitConverter.SingleToInt32Bits(leftFloat) == BitConverter.SingleToInt32Bits(rightFloat),
                (double leftDouble, double rightDouble) =>
                    BitConverter.DoubleToInt64Bits(leftDouble) == BitConverter.DoubleToInt64Bits(rightDouble),
                (string leftString, string rightString) =>
                    leftString.Equals(rightString, StringComparison.Ordinal),
                _ => Equals(leftValue, rightValue),
            };
            if (!equal)
                return false;
        }
        return true;
    }

    private sealed record Fixture(GameParamSourceSet Sources, PARAMDEF Definition);
}
