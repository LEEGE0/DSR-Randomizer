using System.Security.Cryptography;

namespace DSRRandomizer.RmmBridgeHost;

public static class FileContentIdentity
{
    public static bool AreEqual(string leftPath, string rightPath)
    {
        var left = new FileInfo(leftPath);
        var right = new FileInfo(rightPath);
        if (!left.Exists || !right.Exists || left.Length != right.Length)
        {
            return false;
        }

        var leftHash = Hash(leftPath);
        var rightHash = Hash(rightPath);
        return CryptographicOperations.FixedTimeEquals(leftHash, rightHash);
    }

    private static byte[] Hash(string path)
    {
        using var stream = new FileStream(path, new FileStreamOptions
        {
            Access = FileAccess.Read,
            Mode = FileMode.Open,
            Share = FileShare.ReadWrite | FileShare.Delete,
            Options = FileOptions.SequentialScan
        });
        return SHA256.HashData(stream);
    }
}
