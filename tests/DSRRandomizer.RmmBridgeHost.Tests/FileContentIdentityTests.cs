using DSRRandomizer.RmmBridgeHost;

namespace DSRRandomizer.RmmBridgeHost.Tests;

public sealed class FileContentIdentityTests
{
    [Fact]
    public void AreEqual_AcceptsCopiedBinaryAndRejectsDifferentContent()
    {
        var root = Path.Combine(Path.GetTempPath(), $"rmm-bridge-identity-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            var live = Path.Combine(root, "live.exe");
            var copied = Path.Combine(root, "copied.exe");
            var different = Path.Combine(root, "different.exe");
            File.WriteAllBytes(live, [1, 2, 3, 4]);
            File.WriteAllBytes(copied, [1, 2, 3, 4]);
            File.WriteAllBytes(different, [1, 2, 3, 5]);

            Assert.True(FileContentIdentity.AreEqual(live, copied));
            Assert.False(FileContentIdentity.AreEqual(live, different));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
