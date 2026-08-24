namespace DSRRandomizer.Foundation.Safety;

public sealed record CompatibilityProfile(
    string Id,
    ExecutableIdentity Executable,
    long FixedSaveLength,
    ushort ProtocolVersion);

public sealed class UnsupportedGameBuildException : Exception
{
    public UnsupportedGameBuildException(string message)
        : base(message)
    {
    }
}
