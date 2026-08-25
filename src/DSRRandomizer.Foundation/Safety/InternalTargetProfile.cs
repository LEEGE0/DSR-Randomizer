namespace DSRRandomizer.Foundation.Safety;

public enum InternalTargetAction
{
    ForceOffline,
    DenyCall
}

public sealed record InternalTargetProfile(
    string Module,
    uint Rva,
    string FingerprintSha256,
    int PatchLength,
    InternalTargetAction Action);
