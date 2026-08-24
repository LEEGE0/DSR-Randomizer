namespace DSRRandomizer.Foundation.Saves;

public sealed record FileIdentityAndHash(
    string Identity,
    long Length,
    DateTime LastWriteTimeUtc,
    string Sha256);

public sealed record CreatedFileIdentity(string Identity);

public interface IFileMutationLease : IDisposable
{
    void Verify();
}

public sealed class ProfileSessionAlreadyActiveException(string message, Exception innerException)
    : IOException(message, innerException);

public interface IFileAccess
{
    bool Exists(string path);

    IFileMutationLease AcquireMutationLease(
        string rootPath,
        IReadOnlyCollection<string> directoryPaths);

    IFileMutationLease AcquireSessionLock(string rootPath, string lockPath);

    FileAttributes GetAttributes(string path);

    bool IsSingleLinkFile(string path);

    Stream Open(string path, FileMode mode, FileAccess access, FileShare share);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken);

    Task<CreatedFileIdentity> CopyAndFlushAsync(
        Stream source,
        string destinationPath,
        CancellationToken cancellationToken);

    // The implementation owns any created file until this returns successfully.
    // On failure it removes only the object created by this call, never a path replacement.
    Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(
        string path,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken);

    bool MoveCreateNewIfIdentityMatches(
        string sourcePath,
        string destinationPath,
        string expectedSourceIdentity);

    bool ReplaceIfSourceIdentityMatches(
        string sourcePath,
        string destinationPath,
        string expectedSourceIdentity);

    bool DeleteIfIdentityMatches(string path, string expectedIdentity);
}
