using System.Reflection;
using DSRRandomizer.RmmBridgeHost;

namespace DSRRandomizer.RmmBridgeHost.Tests;

public sealed class WindowsBridgeHostPlatformTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        $"rmm-bridge-platform-{Guid.NewGuid():N}");

    [Fact]
    public void SelectedRuntimeImageLease_AcceptsOnlyTheExactCanonicalRuntimePath()
    {
        Directory.CreateDirectory(_root);
        var selectedRoot = Path.Combine(_root, "selected");
        var otherRoot = Path.Combine(_root, "other");
        Directory.CreateDirectory(selectedRoot);
        Directory.CreateDirectory(otherRoot);
        var selected = Path.Combine(selectedRoot, "DarkSoulsRemastered.exe");
        var identicalOtherPath = Path.Combine(otherRoot, "DarkSoulsRemastered.exe");
        File.WriteAllBytes(selected, [1, 2, 3, 4]);
        File.WriteAllBytes(identicalOtherPath, [1, 2, 3, 4]);

        using var accepted = TryLeaseSelectedRuntimeImage(selected, selected);
        using var rejected = TryLeaseSelectedRuntimeImage(identicalOtherPath, selected);

        Assert.NotNull(accepted);
        Assert.Null(rejected);
    }

    [Fact]
    public void SelectedRuntimeImageLease_MissingExpectedImageFailsClosed()
    {
        Directory.CreateDirectory(_root);
        var missing = Path.Combine(_root, "DarkSoulsRemastered.exe");

        using var lease = TryLeaseSelectedRuntimeImage(missing, missing);

        Assert.Null(lease);
    }

    [Fact]
    public void SelectedRuntimeImageLease_BlocksReplacementUntilVerificationReleasesIt()
    {
        Directory.CreateDirectory(_root);
        var selected = Path.Combine(_root, "DarkSoulsRemastered.exe");
        var replacement = Path.Combine(_root, "replacement.exe");
        File.WriteAllBytes(selected, [1, 2, 3, 4]);
        File.WriteAllBytes(replacement, [5, 6, 7, 8]);

        var lease = TryLeaseSelectedRuntimeImage(selected, selected);
        Assert.NotNull(lease);
        try
        {
            var exception = Record.Exception(() => File.Move(replacement, selected, overwrite: true));
            Assert.True(
                exception is IOException or UnauthorizedAccessException,
                $"Replacement unexpectedly succeeded or failed open: {exception}");
            Assert.Equal(new byte[] { 1, 2, 3, 4 }, File.ReadAllBytes(selected));
        }
        finally
        {
            lease.Dispose();
        }

        File.Move(replacement, selected, overwrite: true);
        Assert.Equal(new byte[] { 5, 6, 7, 8 }, File.ReadAllBytes(selected));
    }

    private static FileStream? TryLeaseSelectedRuntimeImage(
        string processImagePath,
        string expectedImagePath)
    {
        var method = typeof(WindowsBridgeHostPlatform).GetMethod(
            "TryLeaseSelectedRuntimeImage",
            BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(method);
        try
        {
            return (FileStream?)method.Invoke(
                new WindowsBridgeHostPlatform(),
                [processImagePath, expectedImagePath]);
        }
        catch (TargetInvocationException exception)
            when (exception.InnerException is not null)
        {
            throw exception.InnerException;
        }
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }
}
