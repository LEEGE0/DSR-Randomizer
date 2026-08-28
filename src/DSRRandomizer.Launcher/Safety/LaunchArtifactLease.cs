using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Launcher.Safety;

internal sealed class LaunchArtifactLease : IDisposable
{
    private readonly List<SafeFileHandle> _parentHandles;
    private FileStream? _stream;

    private LaunchArtifactLease(
        string path,
        FileStream stream,
        List<SafeFileHandle> parentHandles,
        byte[] bytes)
    {
        Path = path;
        _stream = stream;
        _parentHandles = parentHandles;
        Bytes = bytes;
        Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }

    public string Path { get; }
    public byte[] Bytes { get; }
    public string Sha256 { get; }

    public static LaunchArtifactLease? TryOpen(string path)
    {
        try
        {
            return Open(path);
        }
        catch (Exception exception) when (
            exception is IOException
                or UnauthorizedAccessException
                or ArgumentException
                or NotSupportedException)
        {
            return null;
        }
    }

    private static LaunchArtifactLease Open(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = System.IO.Path.GetFullPath(path);
        var parents = OpenParentChain(fullPath);
        FileStream? stream = null;
        try
        {
            var handle = OpenFileHandle(fullPath);
            try
            {
                stream = new FileStream(handle, FileAccess.Read, bufferSize: 81920);
            }
            catch
            {
                handle.Dispose();
                throw;
            }

            if (stream.Length <= 0 || stream.Length > int.MaxValue)
            {
                throw new IOException($"Launch artifact has an unsupported length: {fullPath}");
            }
            var bytes = GC.AllocateUninitializedArray<byte>(checked((int)stream.Length));
            stream.ReadExactly(bytes);
            stream.Position = 0;
            return new LaunchArtifactLease(fullPath, stream, parents, bytes);
        }
        catch
        {
            stream?.Dispose();
            DisposeHandles(parents);
            throw;
        }
    }

    public void Dispose()
    {
        _stream?.Dispose();
        _stream = null;
        DisposeHandles(_parentHandles);
    }

    private static List<SafeFileHandle> OpenParentChain(string filePath)
    {
        var root = System.IO.Path.GetPathRoot(filePath)
            ?? throw new IOException($"Launch artifact root is invalid: {filePath}");
        var parent = System.IO.Path.GetDirectoryName(filePath)
            ?? throw new IOException($"Launch artifact parent is invalid: {filePath}");
        var relative = System.IO.Path.GetRelativePath(root, parent);
        var paths = new List<string> { root };
        var current = root;
        if (!relative.Equals(".", StringComparison.Ordinal))
        {
            foreach (var segment in relative.Split(
                         System.IO.Path.DirectorySeparatorChar,
                         StringSplitOptions.RemoveEmptyEntries))
            {
                current = System.IO.Path.Combine(current, segment);
                paths.Add(current);
            }
        }

        var handles = new List<SafeFileHandle>(paths.Count);
        try
        {
            foreach (var path in paths)
            {
                handles.Add(OpenDirectoryHandle(path));
            }
            return handles;
        }
        catch
        {
            DisposeHandles(handles);
            throw;
        }
    }

    private static SafeFileHandle OpenDirectoryHandle(string path)
    {
        var handle = CreateFileW(
            path,
            FileReadAttributes,
            ShareRead | ShareWrite,
            IntPtr.Zero,
            OpenExisting,
            BackupSemantics | OpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            return ThrowOpenFailure(handle, path);
        }
        try
        {
            var information = GetInformation(handle);
            var attributes = (FileAttributes)information.FileAttributes;
            if ((attributes & FileAttributes.Directory) == 0
                || (attributes & FileAttributes.ReparsePoint) != 0
                || !ResolveFinalPath(handle).Equals(
                    System.IO.Path.GetFullPath(path).TrimEnd(System.IO.Path.DirectorySeparatorChar),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new UnauthorizedAccessException(
                    $"Launch artifact parent is redirected or not a regular directory: {path}");
            }
            return handle;
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    private static SafeFileHandle OpenFileHandle(string path)
    {
        var handle = CreateFileW(
            path,
            GenericRead,
            ShareRead,
            IntPtr.Zero,
            OpenExisting,
            SequentialScan | OpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            return ThrowOpenFailure(handle, path);
        }
        try
        {
            var information = GetInformation(handle);
            var attributes = (FileAttributes)information.FileAttributes;
            if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0
                || information.NumberOfLinks != 1
                || !ResolveFinalPath(handle).Equals(
                    System.IO.Path.GetFullPath(path),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new UnauthorizedAccessException(
                    $"Launch artifact is linked, redirected, or not a private regular file: {path}");
            }
            return handle;
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    private static SafeFileHandle ThrowOpenFailure(SafeFileHandle handle, string path)
    {
        var error = Marshal.GetLastWin32Error();
        handle.Dispose();
        throw new IOException(
            $"Unable to lock launch artifact path: {path}",
            new System.ComponentModel.Win32Exception(error));
    }

    private static ByHandleFileInformation GetInformation(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandle(handle, out var information))
        {
            throw new IOException(
                "Unable to inspect launch artifact identity.",
                new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        }
        return information;
    }

    private static string ResolveFinalPath(SafeFileHandle handle)
    {
        var capacity = 512;
        while (true)
        {
            var buffer = new StringBuilder(capacity);
            var length = GetFinalPathNameByHandleW(handle, buffer, (uint)capacity, 0);
            if (length == 0)
            {
                throw new IOException(
                    "Unable to resolve launch artifact identity.",
                    new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
            }
            if (length < capacity)
            {
                var value = buffer.ToString();
                const string uncPrefix = @"\\?\UNC\";
                const string devicePrefix = @"\\?\";
                if (value.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
                {
                    value = @"\\" + value[uncPrefix.Length..];
                }
                else if (value.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase))
                {
                    value = value[devicePrefix.Length..];
                }
                return value.TrimEnd(System.IO.Path.DirectorySeparatorChar);
            }
            capacity = checked((int)length + 1);
        }
    }

    private static void DisposeHandles(List<SafeFileHandle> handles)
    {
        for (var index = handles.Count - 1; index >= 0; index--)
        {
            handles[index].Dispose();
        }
        handles.Clear();
    }

    private const uint GenericRead = 0x80000000;
    private const uint FileReadAttributes = 0x00000080;
    private const uint ShareRead = 0x00000001;
    private const uint ShareWrite = 0x00000002;
    private const uint OpenExisting = 3;
    private const uint SequentialScan = 0x08000000;
    private const uint BackupSemantics = 0x02000000;
    private const uint OpenReparsePoint = 0x00200000;

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

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation fileInformation);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file,
        StringBuilder filePath,
        uint filePathLength,
        uint flags);
}
