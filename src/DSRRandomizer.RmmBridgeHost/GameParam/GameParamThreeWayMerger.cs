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

            counts.ChangedEntries++;
            if (targetFiles.TryGetValue(id, out BinderFile? existingTarget))
            {
                existingTarget.Bytes = MergeChangedEntry(
                    baseFile,
                    randomFile,
                    existingTarget,
                    inputs.Paramdefs,
                    counts);
            }
            else
            {
                BinderFile added = CloneBinderFile(randomFile);
                inputs.TargetBnd.Files.Add(added);
                targetFiles.Add(id, added);
                changedTargetLayout = true;
            }
        }

        foreach ((int id, BinderFile randomFile) in randomFiles)
        {
            if (baseFiles.ContainsKey(id))
                continue;

            counts.ChangedEntries++;
            if (targetFiles.TryGetValue(id, out BinderFile? targetFile))
            {
                targetFile.Bytes = [.. randomFile.Bytes];
            }
            else
            {
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

    private static byte[] MergeChangedEntry(
        BinderFile baseFile,
        BinderFile randomFile,
        BinderFile targetFile,
        IReadOnlyCollection<PARAMDEF> paramdefs,
        MergeCounts counts)
    {
        bool baseIsParam = HasParamExtension(baseFile);
        bool randomIsParam = HasParamExtension(randomFile);
        bool targetIsParam = HasParamExtension(targetFile);

        if (!baseIsParam && !randomIsParam && !targetIsParam)
            return [.. randomFile.Bytes];

        if (!baseIsParam || !randomIsParam || !targetIsParam)
            throw new InvalidDataException($"Binder entry {baseFile.ID} does not have a compatible PARAMDEF/PARAM payload across all inputs.");

        PARAM baseParam = ReadParam(baseFile, "base");
        PARAM randomParam = ReadParam(randomFile, "randomized");
        PARAM targetParam = ReadParam(targetFile, "target");
        PARAMDEF def = ResolveCommonDefinition(baseParam, randomParam, targetParam, paramdefs, baseFile.ID);
        baseParam.ApplyParamdef(def);
        randomParam.ApplyParamdef(def);
        targetParam.ApplyParamdef(def);

        Dictionary<int, PARAM.Row> baseRows = IndexRows(baseParam, "base", baseFile.ID);
        Dictionary<int, PARAM.Row> randomRows = IndexRows(randomParam, "randomized", baseFile.ID);
        Dictionary<int, PARAM.Row> targetRows = IndexRows(targetParam, "target", baseFile.ID);

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
        PARAM baseParam,
        PARAM randomParam,
        PARAM targetParam,
        IReadOnlyCollection<PARAMDEF> paramdefs,
        int binderId)
    {
        PARAMDEF[] matches = paramdefs.Where(def =>
                string.Equals(def.ParamType, baseParam.ParamType, StringComparison.Ordinal) &&
                string.Equals(def.ParamType, randomParam.ParamType, StringComparison.Ordinal) &&
                string.Equals(def.ParamType, targetParam.ParamType, StringComparison.Ordinal) &&
                def.GetRowSize() == baseParam.DetectedSize &&
                def.GetRowSize() == randomParam.DetectedSize &&
                def.GetRowSize() == targetParam.DetectedSize)
            .ToArray();

        if (matches.Length != 1)
        {
            throw new InvalidDataException(
                $"Binder entry {binderId} requires exactly one compatible PARAMDEF selected by exact ParamType and detected row size; " +
                $"found {matches.Length} for base {baseParam.ParamType}/{baseParam.DetectedSize}, " +
                $"randomized {randomParam.ParamType}/{randomParam.DetectedSize}, and target {targetParam.ParamType}/{targetParam.DetectedSize}.");
        }

        return matches[0];
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

    private static BinderFile CloneBinderFile(BinderFile source) => new(
        source.Flags,
        source.ID,
        source.Name,
        [.. source.Bytes])
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
}
