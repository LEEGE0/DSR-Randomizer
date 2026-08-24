using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Foundation.Saves;

public sealed class SystemFileAccess : IFileAccess
{
    public bool Exists(string path) => File.Exists(path);

    public void CreateDirectory(string path) => Directory.CreateDirectory(path);

    public FileAttributes GetAttributes(string path) => File.GetAttributes(path);

    public Stream Open(string path, FileMode mode, FileAccess access, FileShare share) =>
        new FileStream(
            path,
            mode,
            access,
            share,
            bufferSize: 81920,
            FileOptions.Asynchronous | FileOptions.SequentialScan);

    public async Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (stream is not FileStream fileStream)
        {
            throw new ArgumentException("A file stream is required for identity verification.", nameof(stream));
        }

        if (!stream.CanSeek)
        {
            throw new ArgumentException("A seekable stream is required for hashing.", nameof(stream));
        }

        var information = GetInformation(fileStream.SafeFileHandle);
        stream.Position = 0;
        var hash = await SHA256.HashDataAsync(stream, cancellationToken);
        stream.Position = 0;
        return new FileIdentityAndHash(
            $"{information.VolumeSerialNumber:x8}:{information.FileIndexHigh:x8}{information.FileIndexLow:x8}",
            Combine(information.FileSizeHigh, information.FileSizeLow),
            DateTime.FromFileTimeUtc(Combine(information.LastWriteTimeHigh, information.LastWriteTimeLow)),
            Convert.ToHexString(hash).ToLowerInvariant());
    }

    public async Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return await IdentityAndHashAsync(stream, cancellationToken);
    }

    public async Task CopyAndFlushAsync(
        Stream source,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        source.Position = 0;
        await using var destination = new FileStream(
            destinationPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 81920,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        await source.CopyToAsync(destination, cancellationToken);
        await destination.FlushAsync(cancellationToken);
        destination.Flush(flushToDisk: true);
    }

    public async Task WriteAllBytesAndFlushAsync(
        string path,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 4096,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        await stream.WriteAsync(bytes, cancellationToken);
        await stream.FlushAsync(cancellationToken);
        stream.Flush(flushToDisk: true);
    }

    public void MoveCreateNew(string sourcePath, string destinationPath) =>
        File.Move(sourcePath, destinationPath, overwrite: false);

    public void Replace(string sourcePath, string destinationPath) =>
        File.Move(sourcePath, destinationPath, overwrite: true);

    public void Delete(string path) => File.Delete(path);

    private static ByHandleFileInformation GetInformation(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandle(handle, out var information))
        {
            throw new IOException(
                "Unable to read file identity.",
                new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        }

        return information;
    }

    private static long Combine(uint high, uint low) =>
        unchecked((long)(((ulong)high << 32) | low));

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public uint CreationTimeLow;
        public uint CreationTimeHigh;
        public uint LastAccessTimeLow;
        public uint LastAccessTimeHigh;
        public uint LastWriteTimeLow;
        public uint LastWriteTimeHigh;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation fileInformation);
}
