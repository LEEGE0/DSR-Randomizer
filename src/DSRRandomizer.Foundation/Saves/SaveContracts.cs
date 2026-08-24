namespace DSRRandomizer.Foundation.Saves;

public sealed record SaveProfileCandidate(string SteamId, string SourcePath);

public sealed record DedicatedSaveMetadata(
    int SchemaVersion,
    string SteamId,
    long FixedLength,
    string LastKnownSha256,
    string? ActiveSeedId,
    string? PlacementSha256,
    bool CleanExit);

public sealed record SeedBinding(string SeedId, string PlacementSha256);

public sealed record DedicatedSaveResult(
    bool Ready,
    bool ReusedExisting,
    string? SavePath,
    SaveErrorCode ErrorCode,
    string Message)
{
    public static DedicatedSaveResult Fail(SaveErrorCode code, string message = "") =>
        new(false, false, null, code, message);
}

public enum SaveErrorCode
{
    None,
    InvalidSteamId,
    SourceMissing,
    MultipleProfilesRequireSelection,
    ExistingSaveInvalid,
    CopyVerificationFailed,
    SourceChanged,
    DestinationRace,
    SeedMismatch,
    PathDenied,
    FirstCopyConfirmationRequired
}
