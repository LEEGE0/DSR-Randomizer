using System.IO;
using DSRRandomizer.RmmBridgeHost.GameParam;
using SoulsFormats;

namespace DSRRandomizer.RmmBridgeHost.Tests.GameParam;

public sealed class GameParamThreeWayMergerTests
{
    private const int ParamFileId = 10;
    private const string ParamFileName = "param/TestParam.param";

    [Fact]
    public void Merge_applies_randomizer_row_deltas_and_preserves_unrelated_target_rows()
    {
        PARAMDEF def = CreateDefinition();
        BND3 baseBnd = CreateBinder(CreateParamFile(def,
            Row(1, "base-one", 10),
            Row(2, "base-two", 20),
            Row(3, "base-three", 30),
            Row(4, "base-four", 40)));
        BND3 randomBnd = CreateBinder(CreateParamFile(def,
            Row(1, "random-name-only", 10),
            Row(2, "random-two", 22),
            Row(4, "base-four", 40),
            Row(5, "random-five", 50)));
        BND3 targetBnd = CreateBinder(CreateParamFile(def,
            Row(1, "target-one", 11),
            Row(2, "target-two", 21),
            Row(3, "target-three", 31),
            Row(6, "target-six", 60)));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(baseBnd, randomBnd, targetBnd, [def]));

        PARAM output = ReadParam(result.OutputBytes, ParamFileId, def);
        Assert.Equal([1, 2, 5, 6], output.Rows.Select(row => row.ID));
        AssertRow(output, 1, "target-one", 11);
        AssertRow(output, 2, "target-two", 22);
        AssertRow(output, 5, "random-five", 50);
        AssertRow(output, 6, "target-six", 60);
        Assert.Null(output[3]);
        Assert.Null(output[4]);
        Assert.Equal(1, result.ChangedEntries);
        Assert.Equal(1, result.AddedRows);
        Assert.Equal(1, result.ChangedRows);
        Assert.Equal(1, result.DeletedRows);
        Assert.Equal(2, result.PreservedTargetRows);
        Assert.Equal(2, result.RandomizerWinsOverlaps);
    }

    [Fact]
    public void Merge_uses_bit_exact_cell_comparison_and_copies_array_payloads()
    {
        PARAMDEF def = CreateDefinition();
        RowData baseRow = Row(1, "base", 10) with
        {
            FloatValue = 0.0f,
            DoubleValue = 0.0d,
            Blob = [7, 8, 9],
            Text = "Exact"
        };
        RowData randomRow = baseRow with
        {
            Name = "random",
            FloatValue = BitConverter.Int32BitsToSingle(unchecked((int)0x80000000)),
            DoubleValue = BitConverter.Int64BitsToDouble(unchecked((long)0x8000000000000000)),
            Blob = [7, 8, 9]
        };
        RowData sentinel = Row(99, "sentinel", 99);
        BND3 targetBnd = CreateBinder(CreateParamFile(def, baseRow with { Name = "target" }, sentinel));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(CreateParamFile(def, baseRow, sentinel)),
            CreateBinder(CreateParamFile(def, randomRow, sentinel)),
            targetBnd,
            [def]));

        PARAM output = ReadParam(result.OutputBytes, ParamFileId, def);
        PARAM.Row row = Assert.Single(output.Rows, candidate => candidate.ID == 1);
        Assert.Equal(unchecked((int)0x80000000), BitConverter.SingleToInt32Bits((float)row.Cells[1].Value));
        Assert.Equal(unchecked((long)0x8000000000000000), BitConverter.DoubleToInt64Bits((double)row.Cells[2].Value));
        Assert.Equal(new byte[] { 7, 8, 9 }, (byte[])row.Cells[3].Value);
        Assert.Equal("Exact", (string)row.Cells[4].Value);
        Assert.Equal("target", row.Name);
        Assert.Equal(1, result.ChangedRows);
    }

    [Fact]
    public void Merge_ignores_paramdef_data_version_when_type_and_detected_size_match()
    {
        PARAMDEF def = CreateDefinition(dataVersion: 999);
        BND3 baseBnd = CreateBinder(CreateParamFile(def, 10, Row(1, "base", 10), Row(99, "sentinel", 99)));
        BND3 randomBnd = CreateBinder(CreateParamFile(def, 11, Row(1, "random-name", 10), Row(99, "sentinel", 99)));
        BND3 targetBnd = CreateBinder(CreateParamFile(def, 12, Row(1, "target", 12), Row(99, "sentinel", 99)));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(baseBnd, randomBnd, targetBnd, [def]));

        PARAM output = ReadParam(result.OutputBytes, ParamFileId, def);
        Assert.Equal(12, output.ParamdefDataVersion);
        AssertRow(output, 1, "target", 12);
    }

    [Fact]
    public void Merge_preserves_an_unchanged_binder_entry_without_replacing_its_target_bytes()
    {
        byte[] targetBytes = [9, 8, 7, 6];
        BND3 baseBnd = CreateBinder(new BinderFile(Binder.FileFlags.Flag1, 1, "misc.bin", [1, 2, 3]));
        BND3 randomBnd = CreateBinder(new BinderFile(Binder.FileFlags.Flag1, 1, "misc.bin", [1, 2, 3]));
        BND3 targetBnd = CreateBinder(new BinderFile(Binder.FileFlags.Flag1, 1, "misc.bin", targetBytes));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(baseBnd, randomBnd, targetBnd, []));

        Assert.Same(targetBytes, Assert.Single(targetBnd.Files).Bytes);
        Assert.Equal(targetBytes, Assert.Single(BND3.Read(result.OutputBytes).Files).Bytes);
        Assert.Equal(0, result.ChangedEntries);
    }

    [Fact]
    public void Merge_applies_randomizer_binder_additions_changes_and_deletions()
    {
        BND3 baseBnd = CreateBinder(
            new BinderFile(Binder.FileFlags.Flag1, 1, "delete.bin", [1]),
            new BinderFile(Binder.FileFlags.Flag1, 2, "change.bin", [2]),
            new BinderFile(Binder.FileFlags.Flag1, 4, "unchanged.bin", [4]));
        BND3 randomBnd = CreateBinder(
            new BinderFile(Binder.FileFlags.Flag1, 2, "change.bin", [22]),
            new BinderFile(Binder.FileFlags.Flag1, 3, "add.bin", [3]),
            new BinderFile(Binder.FileFlags.Flag1, 4, "unchanged.bin", [4]));
        BND3 targetBnd = CreateBinder(
            new BinderFile(Binder.FileFlags.Flag1, 1, "delete.bin", [11]),
            new BinderFile(Binder.FileFlags.Flag1, 2, "change.bin", [12]),
            new BinderFile(Binder.FileFlags.Flag1, 4, "unchanged.bin", [14]),
            new BinderFile(Binder.FileFlags.Flag1, 5, "target-only.bin", [15]));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(baseBnd, randomBnd, targetBnd, []));

        BND3 output = BND3.Read(result.OutputBytes);
        Assert.Equal([2, 3, 4, 5], output.Files.Select(file => file.ID));
        Assert.Equal(new byte[] { 22 }, output.Files.Single(file => file.ID == 2).Bytes);
        Assert.Equal(new byte[] { 3 }, output.Files.Single(file => file.ID == 3).Bytes);
        Assert.Equal(new byte[] { 14 }, output.Files.Single(file => file.ID == 4).Bytes);
        Assert.Equal(new byte[] { 15 }, output.Files.Single(file => file.ID == 5).Bytes);
        Assert.Equal(3, result.ChangedEntries);
    }

    [Fact]
    public void Merge_fails_closed_on_duplicate_row_ids_in_a_changed_param()
    {
        PARAMDEF def = CreateDefinition();
        BND3 baseBnd = CreateBinder(CreateParamFile(def, Row(1, "base", 10), Row(99, "sentinel", 99)));
        BND3 randomBnd = CreateBinder(CreateParamFile(def,
            Row(1, "first", 11),
            Row(1, "duplicate", 12),
            Row(99, "sentinel", 99)));
        BND3 targetBnd = CreateBinder(CreateParamFile(def, Row(1, "target", 10), Row(99, "sentinel", 99)));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(baseBnd, randomBnd, targetBnd, [def])));

        Assert.Contains("duplicate row ID", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_fails_closed_when_changed_param_types_are_incompatible()
    {
        PARAMDEF def = CreateDefinition();
        PARAMDEF otherDef = CreateDefinition(paramType: "OtherParam");

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                CreateBinder(CreateParamFile(def, Row(1, "base", 10))),
                CreateBinder(CreateParamFile(otherDef, Row(1, "random", 11))),
                CreateBinder(CreateParamFile(def, Row(1, "target", 10))),
                [def, otherDef])));

        Assert.Contains("compatible PARAMDEF", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_fails_closed_when_changed_param_row_widths_are_incompatible()
    {
        PARAMDEF def = CreateDefinition();
        PARAMDEF narrowDef = CreateDefinition(includeExtendedFields: false);

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                CreateBinder(CreateParamFile(def, Row(1, "base", 10))),
                CreateBinder(CreateParamFile(narrowDef, Row(1, "random", 11))),
                CreateBinder(CreateParamFile(def, Row(1, "target", 10))),
                [def, narrowDef])));

        Assert.Contains("compatible PARAMDEF", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_fails_closed_on_duplicate_binder_entry_ids()
    {
        BND3 duplicateBase = CreateBinder(
            new BinderFile(Binder.FileFlags.Flag1, 1, "one.bin", [1]),
            new BinderFile(Binder.FileFlags.Flag1, 1, "duplicate.bin", [2]));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                duplicateBase,
                CreateBinder(new BinderFile(Binder.FileFlags.Flag1, 1, "one.bin", [3])),
                CreateBinder(new BinderFile(Binder.FileFlags.Flag1, 1, "one.bin", [1])),
                [])));

        Assert.Contains("duplicate binder entry", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_preserves_target_bnd3_dcx_format_and_41_entry_layout()
    {
        BinderFile[] baseFiles = Enumerable.Range(0, 41)
            .Select(id => new BinderFile(Binder.FileFlags.Flag1, id, $"entry-{id:D2}.bin", [(byte)id]))
            .ToArray();
        BinderFile[] randomFiles = baseFiles
            .Select(file => new BinderFile(file.Flags, file.ID, file.Name, [.. file.Bytes]))
            .ToArray();
        BinderFile[] targetFiles = baseFiles
            .Select(file => new BinderFile(file.Flags, file.ID, file.Name, [(byte)(file.ID + 41)]))
            .ToArray();
        BND3 targetBnd = CreateBinder(targetFiles);
        targetBnd.Version = "TARGET";
        targetBnd.Compression = new DCX.DcxDfltCompressionInfo(
            DCX.DfltCompressionPreset.DCX_DFLT_10000_24_9);

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(baseFiles),
            CreateBinder(randomFiles),
            targetBnd,
            []));

        Assert.True(DCX.Is(result.OutputBytes));
        Assert.True(BND3.Is(result.OutputBytes));
        BND3 output = BND3.Read(result.OutputBytes);
        Assert.Equal(DCX.Type.DCX_DFLT, output.Compression.Type);
        Assert.Equal("TARGET", output.Version);
        Assert.Equal(41, output.Files.Count);
        Assert.All(output.Files, file =>
            Assert.Equal(new byte[] { (byte)(file.ID + 41) }, file.Bytes));
    }

    private static PARAMDEF CreateDefinition(
        string paramType = "TestParam",
        short dataVersion = 100,
        bool includeExtendedFields = true)
    {
        var def = new PARAMDEF
        {
            ParamType = paramType,
            DataVersion = dataVersion,
        };
        def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.s32, "value"));
        if (includeExtendedFields)
        {
            def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.f32, "floatValue"));
            def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.f64, "doubleValue"));
            def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.dummy8, "blob") { ArrayLength = 3 });
            def.Fields.Add(new PARAMDEF.Field(def, PARAMDEF.DefType.fixstr, "text") { ArrayLength = 8 });
        }
        return def;
    }

    private static RowData Row(int id, string name, int value) =>
        new(id, name, value, 1.25f, 2.5d, [1, 2, 3], "Text");

    private static BinderFile CreateParamFile(PARAMDEF def, params RowData[] rows) =>
        CreateParamFile(def, def.DataVersion, rows);

    private static BinderFile CreateParamFile(PARAMDEF def, short headerDataVersion, params RowData[] rows)
    {
        var param = new PARAM
        {
            ParamType = def.ParamType,
            ParamdefDataVersion = headerDataVersion,
            Rows = [],
        };
        param.ApplyParamdef(def);
        foreach (RowData data in rows)
        {
            var row = new PARAM.Row(data.ID, data.Name, def);
            row.Cells[0].Value = data.Value;
            if (def.Fields.Count > 1)
            {
                row.Cells[1].Value = data.FloatValue;
                row.Cells[2].Value = data.DoubleValue;
                row.Cells[3].Value = data.Blob;
                row.Cells[4].Value = data.Text;
            }
            param.Rows.Add(row);
        }

        return new BinderFile(Binder.FileFlags.Flag1, ParamFileId, ParamFileName, param.Write());
    }

    private static BND3 CreateBinder(params BinderFile[] files) => new()
    {
        Version = "TEST",
        Files = [.. files],
    };

    private static PARAM ReadParam(byte[] binderBytes, int fileId, PARAMDEF def)
    {
        BND3 binder = BND3.Read(binderBytes);
        BinderFile file = binder.Files.Single(candidate => candidate.ID == fileId);
        PARAM param = PARAM.Read(file.Bytes);
        Assert.Equal(def.ParamType, param.ParamType);
        Assert.Equal(def.GetRowSize(), param.DetectedSize);
        param.ApplyParamdef(def);
        return param;
    }

    private static void AssertRow(PARAM param, int id, string expectedName, int expectedValue)
    {
        PARAM.Row row = Assert.Single(param.Rows, candidate => candidate.ID == id);
        Assert.Equal(expectedName, row.Name);
        Assert.Equal(expectedValue, (int)row.Cells[0].Value);
    }

    private sealed record RowData(
        int ID,
        string Name,
        int Value,
        float FloatValue,
        double DoubleValue,
        byte[] Blob,
        string Text);
}
