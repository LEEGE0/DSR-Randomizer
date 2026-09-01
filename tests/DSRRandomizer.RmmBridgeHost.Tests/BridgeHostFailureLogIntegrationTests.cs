using System.Reflection;
using System.Diagnostics;
using DSRRandomizer.RmmBridgeHost;

namespace DSRRandomizer.RmmBridgeHost.Tests;

public sealed class BridgeHostFailureLogIntegrationTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        $"dsr-rmm-bridge-host-failure-{Guid.NewGuid():N}");
    private readonly List<string> _junctions = [];

    private string ExternalRoot => Path.Combine(_root, "external");

    [Fact]
    public async Task StartupFailure_WritesDiagnosticUnderExternalRoot()
    {
        Directory.CreateDirectory(ExternalRoot);
        WriteStartupFailure(ExternalRoot, "binding exploded");

        var logPath = Path.Combine(ExternalRoot, "logs", "rmm-bridge-host.log");
        var content = await File.ReadAllTextAsync(logPath);
        Assert.Contains("InvalidOperationException", content, StringComparison.Ordinal);
        Assert.Contains("binding exploded", content, StringComparison.Ordinal);
    }

    [Fact]
    public void StartupFailure_LogsJunctionEscapingExternalRoot_DoesNotWriteTarget()
    {
        Directory.CreateDirectory(ExternalRoot);
        string outside = Path.Combine(_root, "outside");
        Directory.CreateDirectory(outside);
        CreateJunction(Path.Combine(ExternalRoot, "logs"), outside);

        WriteStartupFailure(ExternalRoot, "must not escape");

        Assert.False(File.Exists(Path.Combine(outside, "rmm-bridge-host.log")));
    }

    [Fact]
    public void GameParamEvent_LogsJunctionEscapingExternalRoot_DoesNotWriteTarget()
    {
        Directory.CreateDirectory(ExternalRoot);
        string outside = Path.Combine(_root, "outside");
        Directory.CreateDirectory(outside);
        CreateJunction(Path.Combine(ExternalRoot, "logs"), outside);

        WriteGameParamEvent(ExternalRoot, "must not escape");

        Assert.False(File.Exists(Path.Combine(outside, "rmm-bridge-host.log")));
    }

    [Fact]
    public void GameParamEvent_ContainedLogsJunction_DoesNotWriteThroughReparsePoint()
    {
        Directory.CreateDirectory(ExternalRoot);
        string target = Path.Combine(ExternalRoot, "actual-logs");
        Directory.CreateDirectory(target);
        CreateJunction(Path.Combine(ExternalRoot, "logs"), target);

        WriteGameParamEvent(ExternalRoot, "must reject reparse");

        Assert.False(File.Exists(Path.Combine(target, "rmm-bridge-host.log")));
    }

    public void Dispose()
    {
        foreach (string junction in _junctions.OrderByDescending(static path => path.Length))
        {
            if (Directory.Exists(junction))
                Directory.Delete(junction);
        }
        if (Directory.Exists(_root))
            Directory.Delete(_root, recursive: true);
    }

    private static void WriteGameParamEvent(string externalRoot, string message)
    {
        Type loggerType = typeof(BridgeHostArguments).Assembly.GetType(
            "DSRRandomizer.RmmBridgeHost.BridgeHostFailureLog")!;
        MethodInfo write = loggerType.GetMethod(
            "WriteGameParamEvent",
            BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)!;
        write.Invoke(null, [externalRoot, message]);
    }

    private static void WriteStartupFailure(string externalRoot, string message)
    {
        Type loggerType = typeof(BridgeHostArguments).Assembly.GetType(
            "DSRRandomizer.RmmBridgeHost.BridgeHostFailureLog")!;
        MethodInfo write = loggerType.GetMethod(
            "Write",
            BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)!;
        string[] arguments =
        [
            "--game-pid", "4242",
            "--external-root", externalRoot,
            "--runtime-id", "runtime-a39cb5e0",
            "--steam-id", "424242424",
            "--ready-event", @"Local\DSRRandomizer.RmmBridge.0123456789abcdef0123456789abcdef"
        ];
        write.Invoke(null, [arguments, new InvalidOperationException(message)]);
    }

    private void CreateJunction(string junction, string target)
    {
        var start = new ProcessStartInfo("cmd.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
        };
        start.ArgumentList.Add("/c");
        start.ArgumentList.Add("mklink");
        start.ArgumentList.Add("/J");
        start.ArgumentList.Add(junction);
        start.ArgumentList.Add(target);
        using Process process = Process.Start(start)!;
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(process.StandardError.ReadToEnd());
        _junctions.Add(junction);
    }
}
