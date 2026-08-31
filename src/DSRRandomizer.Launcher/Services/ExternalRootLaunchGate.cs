using System.Security.Cryptography;
using System.Text;

namespace DSRRandomizer.Launcher.Services;

internal sealed class ExternalRootLaunchGate : IDisposable
{
    private readonly FileStream _lockStream;

    private ExternalRootLaunchGate(FileStream lockStream)
    {
        _lockStream = lockStream;
    }

    internal static ExternalRootLaunchGate? TryAcquire(string externalRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        var canonicalRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(externalRoot))
            .ToUpperInvariant();
        var identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(canonicalRoot)))
            .ToLowerInvariant();
        var lockRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "DSR-Randomizer",
            "launch-locks");
        Directory.CreateDirectory(lockRoot);
        var lockPath = Path.Combine(lockRoot, $"{identity}.lock");
        try
        {
            return new ExternalRootLaunchGate(new FileStream(
                lockPath,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.None,
                bufferSize: 1,
                FileOptions.DeleteOnClose));
        }
        catch (IOException)
        {
            return null;
        }
    }

    public void Dispose() => _lockStream.Dispose();
}
