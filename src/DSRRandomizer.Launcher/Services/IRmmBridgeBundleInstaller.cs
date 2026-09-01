namespace DSRRandomizer.Launcher.Services;

internal sealed record RmmBridgeInstallResult(bool IsReady, bool Changed, string? ErrorCode)
{
    public static RmmBridgeInstallResult Ready(bool changed) => new(true, changed, null);

    public static RmmBridgeInstallResult Failed(string errorCode) => new(false, false, errorCode);
}

internal interface IRmmBridgeBundleInstaller
{
    RmmBridgeInstallResult EnsureInstalled(string externalRoot);
}
