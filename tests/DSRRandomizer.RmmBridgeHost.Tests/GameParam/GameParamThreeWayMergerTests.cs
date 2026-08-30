using System.IO;
using DSRRandomizer.RmmBridgeHost.GameParam;
using SoulsFormats;

namespace DSRRandomizer.RmmBridgeHost.Tests.GameParam;

public sealed class GameParamThreeWayMergerTests
{
    private const int ParamFileId = 10;
    private const string ParamFileName = "param/TestParam.param";
    private static readonly (int ID, string Name)[] ActualOverhaulGameParamManifest =
    [
        (0, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\EquipParamWeapon.param"),
        (1, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\EquipParamProtector.param"),
        (2, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\EquipParamAccessory.param"),
        (3, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\EquipParamGoods.param"),
        (4, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ReinforceParamWeapon.param"),
        (5, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ReinforceParamProtector.param"),
        (6, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\EquipMtrlSetParam.param"),
        (7, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\default_EnemyBehaviorBank.param"),
        (8, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\default_AIStandardInfoBank.param"),
        (9, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ThrowParam.param"),
        (10, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\BehaviorParam.param"),
        (11, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\BehaviorParam_PC.param"),
        (12, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\NpcParam.param"),
        (13, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\AtkParam_Pc.param"),
        (14, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\AtkParam_Npc.param"),
        (15, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\Magic.param"),
        (16, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\NpcThinkParam.param"),
        (17, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ObjectParam.param"),
        (18, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\Bullet.param"),
        (19, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\SpEffectParam.param"),
        (20, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\SpEffectVfxParam.param"),
        (21, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\TalkParam.param"),
        (22, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\MenuColorTableParam.param"),
        (23, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ItemLotParam.param"),
        (24, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\MoveParam.param"),
        (25, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\CharaInitParam.param"),
        (26, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\FaceGenParam.param"),
        (27, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\RagdollParam.param"),
        (28, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ShopLineupParam.param"),
        (29, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\QwcChange.param"),
        (30, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\QwcJudge.param"),
        (31, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\GameAreaParam.param"),
        (32, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\SkeletonParam.param"),
        (33, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\CalcCorrectGraph.param"),
        (34, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\LockCamParam.param"),
        (35, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\ObjActParam.param"),
        (36, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\HitMtrlParam.param"),
        (37, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\KnockBackParam.param"),
        (38, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\CoolTimeParam.param"),
        (39, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\WhiteCoolTimeParam.param"),
        (40, @"N:\FRPG\data\INTERROOT_x64\param\GameParam\LevelSyncParam.param"),
    ];

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
    public void Merge_preserves_exact_target_param_bytes_and_order_when_only_randomized_row_names_differ()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile targetParam = CreateParamFile(def,
            Row(1, "target-one", 11),
            Row(2, "target-two", 20));
        byte[] targetParamBytes = targetParam.Bytes;
        BND3 targetBnd = CreateBinder(
            new BinderFile(Binder.FileFlags.Flag1, 50, "target-only.bin", [50]),
            targetParam);

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(CreateParamFile(def, Row(1, "base-one", 10), Row(2, "base-two", 20))),
            CreateBinder(CreateParamFile(def, Row(1, "renamed-one", 10), Row(2, "renamed-two", 20))),
            targetBnd,
            [def]));

        Assert.Equal(0, result.ChangedEntries);
        Assert.Same(targetParamBytes, targetParam.Bytes);
        Assert.Equal([50, ParamFileId], targetBnd.Files.Select(file => file.ID));
        BND3 output = BND3.Read(result.OutputBytes);
        Assert.Equal([50, ParamFileId], output.Files.Select(file => file.ID));
        Assert.Equal(targetParamBytes, output.Files.Single(file => file.ID == ParamFileId).Bytes);
        PARAM outputParam = ReadParam(result.OutputBytes, ParamFileId, def);
        AssertRow(outputParam, 1, "target-one", 11);
    }

    [Fact]
    public void Merge_matches_name_only_param_delta_by_normalized_basename_and_preserves_target_layout()
    {
        PARAMDEF lockCamDef = CreateDefinition(paramType: "LockCamParam");
        PARAMDEF hitMtrlDef = CreateDefinition(paramType: "HitMtrlParam");
        BinderFile targetHitMtrl = CreateParamFile(hitMtrlDef, 36,
            @"N:\FRPG\data\INTERROOT_x64\param\GameParam\HitMtrlParam.param",
            Row(1, "target-hit", 70), Row(99, "sentinel", 99));
        BinderFile targetLockCam = CreateParamFile(lockCamDef, 34,
            @"N:\FRPG\data\INTERROOT_x64\param\GameParam\LOCKCAMPARAM.PARAM",
            Row(1, "target-lock", 15), Row(99, "sentinel", 99));
        byte[] targetHitMtrlBytes = targetHitMtrl.Bytes;
        byte[] targetLockCamBytes = targetLockCam.Bytes;
        BND3 targetBnd = CreateBinder(targetHitMtrl, targetLockCam);

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(CreateParamFile(lockCamDef, 36, "param/LockCamParam.param",
                Row(1, "base-lock", 10), Row(99, "sentinel", 99))),
            CreateBinder(CreateParamFile(lockCamDef, 36, @"other\lockcamparam.param",
                Row(1, "renamed-lock", 10), Row(99, "sentinel", 99))),
            targetBnd,
            [lockCamDef, hitMtrlDef]));

        Assert.Equal(0, result.ChangedEntries);
        Assert.Equal([36, 34], targetBnd.Files.Select(file => file.ID));
        Assert.Same(targetHitMtrlBytes, targetHitMtrl.Bytes);
        Assert.Same(targetLockCamBytes, targetLockCam.Bytes);
        BND3 output = BND3.Read(result.OutputBytes);
        Assert.Equal([36, 34], output.Files.Select(file => file.ID));
        Assert.Equal(targetHitMtrlBytes, output.Files[0].Bytes);
        Assert.Equal(targetLockCamBytes, output.Files[1].Bytes);
    }

    [Fact]
    public void Merge_applies_changed_param_by_basename_and_retains_target_entry_metadata()
    {
        PARAMDEF lockCamDef = CreateDefinition(paramType: "LockCamParam");
        PARAMDEF hitMtrlDef = CreateDefinition(paramType: "HitMtrlParam");
        const string targetLockCamName = @"N:\FRPG\data\INTERROOT_x64\param\GameParam\LockCamParam.param";
        BinderFile targetLockCam = CreateParamFile(lockCamDef, 34, targetLockCamName,
            Row(1, "target-lock", 15), Row(99, "sentinel", 99));
        BinderFile targetHitMtrl = CreateParamFile(hitMtrlDef, 36,
            @"N:\FRPG\data\INTERROOT_x64\param\GameParam\HitMtrlParam.param",
            Row(1, "target-hit", 70), Row(99, "sentinel", 99));
        byte[] targetHitMtrlBytes = targetHitMtrl.Bytes;

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(CreateParamFile(lockCamDef, 36, "param/LockCamParam.param",
                Row(1, "base-lock", 10), Row(99, "sentinel", 99))),
            CreateBinder(CreateParamFile(lockCamDef, 36, "param/lockcamparam.PARAM",
                Row(1, "random-lock", 12), Row(99, "sentinel", 99))),
            CreateBinder(targetLockCam, targetHitMtrl),
            [lockCamDef, hitMtrlDef]));

        BND3 output = BND3.Read(result.OutputBytes);
        BinderFile outputLockCam = Assert.Single(output.Files, file => file.ID == 34);
        Assert.Equal(targetLockCamName, outputLockCam.Name);
        PARAM lockCam = PARAM.Read(outputLockCam.Bytes);
        lockCam.ApplyParamdef(lockCamDef);
        AssertRow(lockCam, 1, "target-lock", 12);
        Assert.Equal(targetHitMtrlBytes, Assert.Single(output.Files, file => file.ID == 36).Bytes);
        Assert.Equal(1, result.ChangedEntries);
        Assert.Equal(1, result.ChangedRows);
    }

    [Fact]
    public void Merge_applies_param_addition_and_deletion_by_basename_not_numeric_id()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile retainedSameId = CreateParamFile(def, 41, "param/RetainedParam.param",
            Row(1, "retained", 71), Row(99, "sentinel", 99));
        byte[] retainedBytes = retainedSameId.Bytes;

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(CreateParamFile(def, 41, "param/DeletedParam.param",
                Row(1, "base-delete", 10), Row(99, "sentinel", 99))),
            CreateBinder(CreateParamFile(def, 42, "param/AddedParam.param",
                Row(2, "random-add", 20), Row(99, "sentinel", 99))),
            CreateBinder(
                CreateParamFile(def, 39, @"target\DELETEDPARAM.PARAM",
                    Row(1, "target-delete", 11), Row(99, "sentinel", 99)),
                retainedSameId),
            [def]));

        BND3 output = BND3.Read(result.OutputBytes);
        Assert.DoesNotContain(output.Files, file =>
            file.Name!.EndsWith("DeletedParam.param", StringComparison.OrdinalIgnoreCase));
        Assert.Equal(retainedBytes, Assert.Single(output.Files, file => file.ID == 41).Bytes);
        BinderFile added = Assert.Single(output.Files, file => file.ID == 42);
        Assert.Equal("param/AddedParam.param", added.Name);
        PARAM addedParam = PARAM.Read(added.Bytes);
        addedParam.ApplyParamdef(def);
        AssertRow(addedParam, 2, "random-add", 20);
        Assert.Equal(2, result.ChangedEntries);
    }

    [Fact]
    public void Merge_rejects_case_insensitive_duplicate_param_basenames_with_different_ids()
    {
        PARAMDEF def = CreateDefinition();
        BND3 duplicateBase = CreateBinder(
            CreateParamFile(def, 1, @"one\DuplicateParam.param", Row(1, "one", 10)),
            CreateParamFile(def, 2, "two/duplicateparam.PARAM", Row(1, "two", 20)));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                duplicateBase, CreateBinder(), CreateBinder(), [def])));

        Assert.Contains("duplicate logical PARAM", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_target_deleted_param_keeps_only_randomizer_changed_or_added_rows()
    {
        PARAMDEF def = CreateDefinition();
        BND3 baseBnd = CreateBinder(CreateParamFile(def,
            Row(1, "base-equal", 10),
            Row(2, "base-change", 20),
            Row(3, "base-delete", 30)));
        BND3 randomBnd = CreateBinder(CreateParamFile(def,
            Row(1, "random-name-only", 10),
            Row(2, "random-change", 22),
            Row(4, "random-add", 40)));

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(baseBnd, randomBnd, CreateBinder(), [def]));

        PARAM output = ReadParam(result.OutputBytes, ParamFileId, def);
        Assert.Equal([2, 4], output.Rows.Select(row => row.ID));
        AssertRow(output, 2, "random-change", 22);
        AssertRow(output, 4, "random-add", 40);
        Assert.Null(output[1]);
        Assert.Null(output[3]);
        Assert.Equal(1, result.ChangedEntries);
        Assert.Equal(1, result.AddedRows);
        Assert.Equal(1, result.ChangedRows);
        Assert.Equal(1, result.DeletedRows);
        Assert.Equal(1, result.RandomizerWinsOverlaps);
    }

    [Fact]
    public void Merge_rejects_malformed_added_param_when_base_and_target_are_absent()
    {
        BinderFile malformed = new(Binder.FileFlags.Flag1, ParamFileId, ParamFileName, [1, 2, 3]);

        Assert.Throws<InvalidDataException>(() => new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(CreateBinder(), CreateBinder(malformed), CreateBinder(), [])));
    }

    [Fact]
    public void Merge_rejects_duplicate_rows_in_added_param_when_base_and_target_are_absent()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile duplicate = CreateParamFile(def, Row(1, "first", 10), Row(1, "duplicate", 11));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                CreateBinder(), CreateBinder(duplicate), CreateBinder(), [def])));

        Assert.Contains("duplicate row ID", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Merge_rejects_malformed_changed_param_when_target_is_absent()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile malformed = new(Binder.FileFlags.Flag1, ParamFileId, ParamFileName, [1, 2, 3]);

        Assert.Throws<InvalidDataException>(() => new GameParamThreeWayMerger().Merge(
            new GameParamMergeInputs(
                CreateBinder(CreateParamFile(def, Row(1, "base", 10), Row(2, "sentinel", 20))),
                CreateBinder(malformed),
                CreateBinder(),
                [def])));
    }

    [Fact]
    public void Merge_rejects_duplicate_rows_in_changed_param_when_target_is_absent()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile duplicate = CreateParamFile(def,
            Row(1, "first", 11),
            Row(1, "duplicate", 12),
            Row(2, "sentinel", 20));

        InvalidDataException exception = Assert.Throws<InvalidDataException>(() =>
            new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
                CreateBinder(CreateParamFile(def, Row(1, "base", 10), Row(2, "sentinel", 20))),
                CreateBinder(duplicate),
                CreateBinder(),
                [def])));

        Assert.Contains("duplicate row ID", exception.Message, StringComparison.OrdinalIgnoreCase);
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
    public void Merge_preserves_target_bnd3_dcx_format_and_actual_cross_numbered_41_param_manifest()
    {
        PARAMDEF def = CreateDefinition();
        BinderFile[] baseFiles = ActualOverhaulGameParamManifest
            .Select(entry => CreateParamFile(def, GetBaseRandomBinderId(entry), entry.Name,
                Row(1, $"base-{entry.ID}", entry.ID), Row(99, "sentinel", 99)))
            .ToArray();
        BinderFile[] randomFiles = ActualOverhaulGameParamManifest
            .Select(entry => CreateParamFile(def, GetBaseRandomBinderId(entry), entry.Name,
                Row(1, $"random-name-{entry.ID}",
                    entry.ID == 34 ? 434 : entry.ID),
                Row(99, "sentinel", 99)))
            .ToArray();
        BinderFile[] targetFiles = ActualOverhaulGameParamManifest
            .Select(entry => CreateParamFile(def, entry.ID, entry.Name,
                Row(1, $"target-{entry.ID}", entry.ID + 100), Row(99, "sentinel", 99)))
            .ToArray();
        byte[][] targetPayloads = targetFiles.Select(file => file.Bytes).ToArray();
        BND3 targetBnd = CreateBinder(targetFiles);
        targetBnd.Version = "TARGET";
        targetBnd.Compression = new DCX.DcxDfltCompressionInfo(
            DCX.DfltCompressionPreset.DCX_DFLT_10000_24_9);

        GameParamMergeResult result = new GameParamThreeWayMerger().Merge(new GameParamMergeInputs(
            CreateBinder(baseFiles),
            CreateBinder(randomFiles),
            targetBnd,
            [def]));

        Assert.Equal(1, result.ChangedEntries);
        Assert.True(DCX.Is(result.OutputBytes));
        Assert.True(BND3.Is(result.OutputBytes));
        BND3 output = BND3.Read(result.OutputBytes);
        Assert.Equal(DCX.Type.DCX_DFLT, output.Compression.Type);
        Assert.Equal("TARGET", output.Version);
        Assert.Equal(41, output.Files.Count);
        Assert.Equal(ActualOverhaulGameParamManifest.Select(entry => entry.ID), output.Files.Select(file => file.ID));
        Assert.Equal(ActualOverhaulGameParamManifest.Select(entry => entry.Name), output.Files.Select(file => file.Name));
        for (int index = 0; index < targetFiles.Length; index++)
        {
            if (targetFiles[index].ID == 34)
            {
                PARAM lockCam = PARAM.Read(output.Files[index].Bytes);
                lockCam.ApplyParamdef(def);
                AssertRow(lockCam, 1, "target-34", 434);
            }
            else
            {
                Assert.Same(targetPayloads[index], targetFiles[index].Bytes);
                Assert.Equal(targetPayloads[index], output.Files[index].Bytes);
            }
        }
    }

    private static int GetBaseRandomBinderId((int ID, string Name) entry) =>
        Path.GetFileNameWithoutExtension(entry.Name) switch
        {
            "LockCamParam" => 36,
            "ObjActParam" => 37,
            "HitMtrlParam" => 38,
            "KnockBackParam" => 39,
            "LevelSyncParam" => 40,
            "CoolTimeParam" => 41,
            "WhiteCoolTimeParam" => 42,
            _ => entry.ID,
        };

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
        CreateParamFile(def, ParamFileId, ParamFileName, def.DataVersion, rows);

    private static BinderFile CreateParamFile(PARAMDEF def, short headerDataVersion, params RowData[] rows)
        => CreateParamFile(def, ParamFileId, ParamFileName, headerDataVersion, rows);

    private static BinderFile CreateParamFile(
        PARAMDEF def,
        int fileId,
        string fileName,
        params RowData[] rows)
        => CreateParamFile(def, fileId, fileName, def.DataVersion, rows);

    private static BinderFile CreateParamFile(
        PARAMDEF def,
        int fileId,
        string fileName,
        short headerDataVersion,
        params RowData[] rows)
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

        return new BinderFile(Binder.FileFlags.Flag1, fileId, fileName, param.Write());
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
