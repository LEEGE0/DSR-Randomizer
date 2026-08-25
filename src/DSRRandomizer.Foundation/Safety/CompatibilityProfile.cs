namespace DSRRandomizer.Foundation.Safety;

public sealed record ModuleProfile(
    string Name,
    ExecutableIdentity Identity);

public sealed record CompatibilityProfile(
    string Id,
    string ExecutableModule,
    ExecutableIdentity Executable,
    long FixedSaveLength,
    ushort ProtocolVersion,
    IReadOnlyList<ModuleProfile> Modules,
    IReadOnlyList<InternalTargetProfile> GameServiceTargets)
{
    public CompatibilityProfile(
        string id,
        ExecutableIdentity executable,
        long fixedSaveLength,
        ushort protocolVersion)
        : this(
            id,
            "DarkSoulsRemastered.exe",
            executable,
            fixedSaveLength,
            protocolVersion,
            Array.Empty<ModuleProfile>(),
            Array.Empty<InternalTargetProfile>())
    {
    }
}

public sealed class UnsupportedGameBuildException : Exception
{
    public UnsupportedGameBuildException(string message)
        : base(message)
    {
    }
}

public sealed class CompatibilityProfileFormatException : Exception
{
    public CompatibilityProfileFormatException(string message)
        : base(message)
    {
    }

    public CompatibilityProfileFormatException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
