using DSRRandomizer.Foundation.Safety;

namespace DSRRandomizer.Launcher.Safety;

public sealed record SafetyLaunchRequest(
    string ExecutablePath,
    string WorkingDirectory,
    string GuardDllPath,
    CompatibilityProfile Profile,
    ulong RequiredProtectionFlags,
    bool DiagnosticMode,
    IReadOnlyList<string>? Arguments = null);

public sealed record ProtectionHandshake(
    bool Success,
    ulong ActiveFlags,
    string ErrorCode)
{
    public static ProtectionHandshake Failed(string errorCode) =>
        new(false, 0, errorCode);
}

public sealed record SafetyLaunchResult(
    bool Started,
    string ErrorCode,
    int? ExitCode)
{
    public static SafetyLaunchResult Failed(string errorCode) =>
        new(false, errorCode, null);
}

public sealed class SafetyLaunchException : Exception
{
    public SafetyLaunchException(string errorCode)
        : base(errorCode)
    {
        ErrorCode = errorCode;
    }

    public string ErrorCode { get; }
}
