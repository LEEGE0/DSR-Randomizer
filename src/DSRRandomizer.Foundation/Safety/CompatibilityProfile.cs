namespace DSRRandomizer.Foundation.Safety;

public sealed record ModuleProfile(
    string Name,
    ExecutableIdentity Identity,
    bool AllowDeferred,
    IReadOnlyList<string> DeclaredInterfaces,
    IReadOnlyList<string> ProtectedFactoryExports)
{
    public ModuleProfile(string name, ExecutableIdentity identity)
        : this(name, identity, false, Array.Empty<string>(), Array.Empty<string>())
    {
    }
}

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
