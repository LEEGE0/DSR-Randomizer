using SoulsFormats;

namespace DSRRandomizer.RmmBridgeHost.GameParam;

public sealed record GameParamMergeInputs(
    BND3 BaseBnd,
    BND3 RandomizedBnd,
    BND3 TargetBnd,
    IReadOnlyCollection<PARAMDEF> Paramdefs);

public sealed record GameParamMergeResult(
    byte[] OutputBytes,
    int ChangedEntries,
    int AddedRows,
    int ChangedRows,
    int DeletedRows,
    int PreservedTargetRows,
    int RandomizerWinsOverlaps);
