namespace DSRRandomizer.Foundation.Saves;

public sealed record FileIdentityAndHash(
    string Identity,
    long Length,
    DateTime LastWriteTimeUtc,
    string Sha256);

public interface IFileMutationLease : IDisposable
{
    void Verify();
}

public interface IFileAccess
{
    bool Exists(string path);

    IFileMutationLease AcquireMutationLease(
        string rootPath,
        IReadOnlyCollection<string> directoryPaths);

    FileAttributes GetAttributes(string path);

    Stream Open(string path, FileMode mode, FileAccess access, FileShare share);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken);

    Task CopyAndFlushAsync(
        Stream source,
        string destinationPath,
        CancellationToken cancellationToken);

    Task WriteAllBytesAndFlushAsync(
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
