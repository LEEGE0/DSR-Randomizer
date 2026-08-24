using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Foundation.Tests.Saves;

public sealed class SystemFileAccessTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        "DSRRandomizer.SystemFileAccess.Tests",
        Guid.NewGuid().ToString("N"));

    [Fact]
    public void MutationLease_AllowsNativeMetadataPin()
    {
        var profile = Path.Combine(_root, "saves", "12345678901234567");
        Directory.CreateDirectory(profile);
        var access = new SystemFileAccess();
        using var lease = access.AcquireMutationLease(_root, [profile]);

        using var nativePin = OpenNativeMetadataPin(profile);
        Assert.False(nativePin.IsInvalid);

        lease.Verify();
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    private static SafeFileHandle OpenNativeMetadataPin(string path)
    {
        const uint fileReadAttributes = 0x00000080;
        const uint shareRead = 0x00000001;
        const uint shareWrite = 0x00000002;
        const uint openExisting = 3;
        const uint backupSemantics = 0x02000000;
        const uint openReparsePoint = 0x00200000;
        return CreateFileW(
            path,
            fileReadAttributes,
            shareRead | shareWrite,
            IntPtr.Zero,
            openExisting,
            backupSemantics | openReparsePoint,
            IntPtr.Zero);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

}
