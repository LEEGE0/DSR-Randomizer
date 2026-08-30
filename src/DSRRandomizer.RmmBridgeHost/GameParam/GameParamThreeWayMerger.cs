using System.IO;
using SoulsFormats;

namespace DSRRandomizer.RmmBridgeHost.GameParam;

public sealed class GameParamThreeWayMerger
{
    public GameParamMergeResult Merge(GameParamMergeInputs inputs)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(inputs.BaseBnd);
        ArgumentNullException.ThrowIfNull(inputs.RandomizedBnd);
        ArgumentNullException.ThrowIfNull(inputs.TargetBnd);
        ArgumentNullException.ThrowIfNull(inputs.Paramdefs);

        Dictionary<int, BinderFile> baseFiles = IndexBinder(inputs.BaseBnd, "base");
        Dictionary<int, BinderFile> randomFiles = IndexBinder(inputs.RandomizedBnd, "randomized");
        Dictionary<int, BinderFile> targetFiles = IndexBinder(inputs.TargetBnd, "target");
        var counts = new MergeCounts();
        bool changedTargetLayout = false;

        foreach ((int id, BinderFile baseFile) in baseFiles)
        {
            if (!randomFiles.TryGetValue(id, out BinderFile? randomFile))
            {
                targetFiles.TryGetValue(id, out BinderFile? deletionTarget);
                if (HasParamExtension(baseFile) || deletionTarget is not null && HasParamExtension(deletionTarget))
                {
                    (BinderFile File, string Source)[] deletedParams = deletionTarget is null
                        ? [(baseFile, "base")]
                        : [(baseFile, "base"), (deletionTarget, "target")];
                    ValidateCompatibleParams(
                        deletedParams,
                        inputs.Paramdefs,
                        id);
                }

                counts.ChangedEntries++;
                if (targetFiles.TryGetValue(id, out BinderFile? targetFile))
                {
                    inputs.TargetBnd.Files.Remove(targetFile);
                    targetFiles.Remove(id);
                    changedTargetLayout = true;
                }
                continue;
            }

            if (baseFile.Bytes.AsSpan().SequenceEqual(randomFile.Bytes))
                continue;

            if (targetFiles.TryGetValue(id, out BinderFile? existingTarget))
            {
                if (IsParamEntry(baseFile, randomFile, existingTarget))
                {
                    ParamMergeOutcome outcome = MergeChangedParamEntry(
                        baseFile, randomFile, existingTarget, inputs.Paramdefs, counts);
                    if (!outcome.HasFunctionalChange)
                        continue;

                    counts.ChangedEntries++;
                    existingTarget.Bytes = outcome.OutputBytes!;
                }
                else
                {
                    counts.ChangedEntries++;
                    existingTarget.Bytes = [.. randomFile.Bytes];
                }
            }
            else
            {
                if (IsParamEntry(baseFile, randomFile))
                {
                    ParamMergeOutcome outcome = MergeChangedParamEntry(
                        baseFile, randomFile, null, inputs.Paramdefs, counts);
                    if (!outcome.HasFunctionalChange)
                        continue;

                    counts.ChangedEntries++;
                    if (outcome.ShouldCreateEntry)
                    {
                        BinderFile added = CloneBinderFile(randomFile, outcome.OutputBytes!);
                        inputs.TargetBnd.Files.Add(added);
                        targetFiles.Add(id, added);
                        changedTargetLayout = true;
                    }
                }
                else
                {
                    counts.ChangedEntries++;
                    BinderFile added = CloneBinderFile(randomFile);
                    inputs.TargetBnd.Files.Add(added);
                    targetFiles.Add(id, added);
                    changedTargetLayout = true;
                }
            }
        }

        foreach ((int id, BinderFile randomFile) in randomFiles)
        {
            if (baseFiles.ContainsKey(id))
                continue;

            if (targetFiles.TryGetValue(id, out BinderFile? targetFile))
            {
                if (HasParamExtension(randomFile) || HasParamExtension(targetFile))
                {
                    Dictionary<int, PARAM.Row>[] rows = ValidateCompatibleParams(
                        [(randomFile, "randomized"), (targetFile, "target")],
                        inputs.Paramdefs,
                        id);
                    counts.AddedRows += rows[0].Count;
                }

                counts.ChangedEntries++;
                targetFile.Bytes = [.. randomFile.Bytes];
            }
            else
            {
                if (HasParamExtension(randomFile))
                {
                    Dictionary<int, PARAM.Row>[] rows = ValidateCompatibleParams(
                        [(randomFile, "randomized")],
                        inputs.Paramdefs,
                        id);
                    counts.AddedRows += rows[0].Count;
                }

                counts.ChangedEntries++;
                BinderFile added = CloneBinderFile(randomFile);
                inputs.TargetBnd.Files.Add(added);
                targetFiles.Add(id, added);
                changedTargetLayout = true;
            }
        }

        if (changedTargetLayout)
            inputs.TargetBnd.Files.Sort(static (left, right) => left.ID.CompareTo(right.ID));

        return new GameParamMergeResult(
            inputs.TargetBnd.Write(),
            counts.ChangedEntries,
            counts.AddedRows,
            counts.ChangedRows,
            counts.DeletedRows,
            counts.PreservedTargetRows,
            counts.RandomizerWinsOverlaps);
    }

    private static ParamMergeOutcome MergeChangedParamEntry(
        BinderFile baseFile,
        BinderFile randomFile,
        BinderFile? targetFile,
        IReadOnlyCollection<PARAMDEF> paramdefs,
        MergeCounts counts)
    {
        PARAM baseParam = ReadParam(baseFile, "base");
        PARAM randomParam = ReadParam(randomFile, "randomized");
        PARAM? targetParam = targetFile is null ? null : ReadParam(targetFile, "target");
        (PARAM Param, string Source)[] parsed = targetParam is null
            ? [(baseParam, "base"), (randomParam, "randomized")]
            : [(baseParam, "base"), (randomParam, "randomized"), (targetParam, "target")];
        PARAMDEF def = ResolveCommonDefinition(parsed, paramdefs, baseFile.ID);
        baseParam.ApplyParamdef(def);
        randomParam.ApplyParamdef(def);
        targetParam?.ApplyParamdef(def);

        Dictionary<int, PARAM.Row> baseRows = IndexRows(baseParam, "base", baseFile.ID);
        Dictionary<int, PARAM.Row> randomRows = IndexRows(randomParam, "randomized", baseFile.ID);
        Dictionary<int, PARAM.Row> targetRows = targetParam is null
            ? []
            : IndexRows(targetParam, "target", baseFile.ID);

        if (RowSetsEqual(baseRows, randomRows))
            return new ParamMergeOutcome(false, false, null);

        if (targetParam is null)
        {
            targetParam = ReadParam(randomFile, "randomized output");
            targetParam.ApplyParamdef(def);
            targetParam.Rows = [];
        }

        byte[] outputBytes = MergeParamRows(targetParam, baseRows, randomRows, targetRows, def, counts);
        return new ParamMergeOutcome(true, targetFile is not null || targetParam.Rows.Count > 0, outputBytes);
    }

    private static byte[] MergeParamRows(
        PARAM targetParam,
        Dictionary<int, PARAM.Row> baseRows,
        Dictionary<int, PARAM.Row> randomRows,
        Dictionary<int, PARAM.Row> targetRows,
        PARAMDEF def,
        MergeCounts counts)
    {
        foreach ((int id, PARAM.Row baseRow) in baseRows)
        {
            if (!randomRows.TryGetValue(id, out PARAM.Row? randomRow))
            {
                counts.DeletedRows++;
                if (targetRows.TryGetValue(id, out PARAM.Row? targetRow))
                {
                    if (!RowsEqual(baseRow, targetRow))
                        counts.RandomizerWinsOverlaps++;
                    targetParam.Rows.Remove(targetRow);
                    targetRows.Remove(id);
                }
                continue;
            }

            if (RowsEqual(baseRow, randomRow))
            {
                if (targetRows.ContainsKey(id))
                    counts.PreservedTargetRows++;
                continue;
            }

            counts.ChangedRows++;
            if (targetRows.TryGetValue(id, out PARAM.Row? existingTargetRow))
            {
                if (!RowsEqual(baseRow, existingTargetRow))
                    counts.RandomizerWinsOverlaps++;
                int targetIndex = targetParam.Rows.IndexOf(existingTargetRow);
                PARAM.Row replacement = CloneRow(randomRow, def, existingTargetRow.Name ?? randomRow.Name);
                targetParam.Rows[targetIndex] = replacement;
                targetRows[id] = replacement;
            }
            else
            {
                counts.RandomizerWinsOverlaps++;
                PARAM.Row replacement = CloneRow(randomRow, def, randomRow.Name);
                targetParam.Rows.Add(replacement);
                targetRows.Add(id, replacement);
            }
        }

        foreach ((int id, PARAM.Row randomRow) in randomRows)
        {
            if (baseRows.ContainsKey(id))
                continue;

            counts.AddedRows++;
            if (targetRows.TryGetValue(id, out PARAM.Row? existingTargetRow))
            {
                counts.RandomizerWinsOverlaps++;
                int targetIndex = targetParam.Rows.IndexOf(existingTargetRow);
                PARAM.Row replacement = CloneRow(randomRow, def, existingTargetRow.Name ?? randomRow.Name);
                targetParam.Rows[targetIndex] = replacement;
                targetRows[id] = replacement;
            }
            else
            {
                PARAM.Row replacement = CloneRow(randomRow, def, randomRow.Name);
                targetParam.Rows.Add(replacement);
                targetRows.Add(id, replacement);
            }
        }

        counts.PreservedTargetRows += targetRows.Keys.Count(id =>
            !baseRows.ContainsKey(id) && !randomRows.ContainsKey(id));
        targetParam.Rows = targetParam.Rows.OrderBy(static row => row.ID).ToList();
        return targetParam.Write();
    }

    private static PARAMDEF ResolveCommonDefinition(
        IReadOnlyCollection<(PARAM Param, string Source)> parsed,
        IReadOnlyCollection<PARAMDEF> paramdefs,
        int binderId)
    {
        PARAMDEF[] matches = paramdefs.Where(def =>
                parsed.All(item =>
                    string.Equals(def.ParamType, item.Param.ParamType, StringComparison.Ordinal) &&
                    def.GetRowSize() == item.Param.DetectedSize))
            .ToArray();

        if (matches.Length != 1)
        {
            string layouts = string.Join(", ", parsed.Select(item =>
                $"{item.Source} {item.Param.ParamType}/{item.Param.DetectedSize}"));
            throw new InvalidDataException(
                $"Binder entry {binderId} requires exactly one compatible PARAMDEF selected by exact ParamType and detected row size; " +
                $"found {matches.Length} for {layouts}.");
        }

        return matches[0];
    }

    private static Dictionary<int, PARAM.Row>[] ValidateCompatibleParams(
        IReadOnlyList<(BinderFile File, string Source)> files,
        IReadOnlyCollection<PARAMDEF> paramdefs,
        int binderId)
    {
        if (files.Any(item => !HasParamExtension(item.File)))
        {
            throw new InvalidDataException(
                $"Binder entry {binderId} does not have compatible PARAM names across all available inputs.");
        }

        (PARAM Param, string Source)[] parsed = files
            .Select(item => (ReadParam(item.File, item.Source), item.Source))
            .ToArray();
        PARAMDEF def = ResolveCommonDefinition(parsed, paramdefs, binderId);
        var indexed = new Dictionary<int, PARAM.Row>[parsed.Length];
        for (int index = 0; index < parsed.Length; index++)
        {
            parsed[index].Param.ApplyParamdef(def);
            indexed[index] = IndexRows(parsed[index].Param, parsed[index].Source, binderId);
        }
        return indexed;
    }

    private static Dictionary<int, BinderFile> IndexBinder(BND3 binder, string source)
    {
        var indexed = new Dictionary<int, BinderFile>();
        foreach (BinderFile file in binder.Files)
        {
            if (!indexed.TryAdd(file.ID, file))
                throw new InvalidDataException($"The {source} binder contains duplicate binder entry ID {file.ID}.");
        }
        return indexed;
    }

    private static Dictionary<int, PARAM.Row> IndexRows(PARAM param, string source, int binderId)
    {
        var indexed = new Dictionary<int, PARAM.Row>();
        foreach (PARAM.Row row in param.Rows)
        {
            if (!indexed.TryAdd(row.ID, row))
            {
                throw new InvalidDataException(
                    $"The {source} PARAM in binder entry {binderId} contains duplicate row ID {row.ID}.");
            }
        }
        return indexed;
    }

    private static bool RowsEqual(PARAM.Row left, PARAM.Row right)
    {
        if (left.Cells.Count != right.Cells.Count)
            return false;

        for (int i = 0; i < left.Cells.Count; i++)
        {
            if (!CellValuesEqual(left.Cells[i].Value, right.Cells[i].Value))
                return false;
        }
        return true;
    }

    private static bool RowSetsEqual(
        IReadOnlyDictionary<int, PARAM.Row> left,
        IReadOnlyDictionary<int, PARAM.Row> right)
    {
        if (left.Count != right.Count)
            return false;

        return left.All(item =>
            right.TryGetValue(item.Key, out PARAM.Row? rightRow) && RowsEqual(item.Value, rightRow));
    }

    private static bool CellValuesEqual(object left, object right) => (left, right) switch
    {
        (byte[] leftBytes, byte[] rightBytes) => leftBytes.AsSpan().SequenceEqual(rightBytes),
        (float leftFloat, float rightFloat) =>
            BitConverter.SingleToInt32Bits(leftFloat) == BitConverter.SingleToInt32Bits(rightFloat),
        (double leftDouble, double rightDouble) =>
            BitConverter.DoubleToInt64Bits(leftDouble) == BitConverter.DoubleToInt64Bits(rightDouble),
        (string leftString, string rightString) =>
            string.Equals(leftString, rightString, StringComparison.Ordinal),
        _ => Equals(left, right),
    };

    private static PARAM.Row CloneRow(PARAM.Row source, PARAMDEF def, string? name)
    {
        var clone = new PARAM.Row(source.ID, name, def);
        for (int i = 0; i < source.Cells.Count; i++)
        {
            clone.Cells[i].Value = source.Cells[i].Value is byte[] bytes
                ? bytes.ToArray()
                : source.Cells[i].Value;
        }
        return clone;
    }

    private static BinderFile CloneBinderFile(BinderFile source, byte[]? bytes = null) => new(
        source.Flags,
        source.ID,
        source.Name,
        bytes ?? [.. source.Bytes])
    {
        CompressionInfo = source.CompressionInfo,
    };

    private static PARAM ReadParam(BinderFile file, string source)
    {
        try
        {
            return PARAM.Read(file.Bytes);
        }
        catch (Exception exception) when (exception is InvalidDataException or FormatException or EndOfStreamException)
        {
            throw new InvalidDataException($"The {source} binder entry {file.ID} is not a readable PARAM.", exception);
        }
    }

    private static bool IsParamEntry(params BinderFile[] files)
    {
        if (files.Any(HasParamExtension))
        {
            if (files.Any(file => !HasParamExtension(file)))
            {
                throw new InvalidDataException(
                    $"Binder entry {files[0].ID} does not have compatible PARAM names across all available inputs.");
            }
            return true;
        }
        return false;
    }

    private static bool HasParamExtension(BinderFile file) =>
        file.Name?.EndsWith(".param", StringComparison.OrdinalIgnoreCase) == true;

    private sealed class MergeCounts
    {
        public int ChangedEntries { get; set; }
        public int AddedRows { get; set; }
        public int ChangedRows { get; set; }
        public int DeletedRows { get; set; }
        public int PreservedTargetRows { get; set; }
        public int RandomizerWinsOverlaps { get; set; }
    }

    private sealed record ParamMergeOutcome(
        bool HasFunctionalChange,
        bool ShouldCreateEntry,
        byte[]? OutputBytes);
}
