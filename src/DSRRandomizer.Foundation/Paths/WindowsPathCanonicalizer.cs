using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Foundation.Paths;

public sealed class WindowsPathCanonicalizer : IPathCanonicalizer
{
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint FileShareDelete = 0x00000004;
    private const uint OpenExisting = 3;
    private const uint FileFlagBackupSemantics = 0x02000000;

    public string Canonicalize(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        var fullPath = Path.GetFullPath(path);
        var missingSegments = new Stack<string>();
        var existingAncestor = fullPath;

        while (!File.Exists(existingAncestor) && !Directory.Exists(existingAncestor))
        {
            var trimmed = Path.TrimEndingDirectorySeparator(existingAncestor);
            var segment = Path.GetFileName(trimmed);
            var parent = Path.GetDirectoryName(trimmed);
            if (string.IsNullOrEmpty(segment) || string.IsNullOrEmpty(parent))
            {
                throw new IOException($"No existing ancestor can be resolved for path: {path}");
            }

            missingSegments.Push(segment);
            existingAncestor = parent;
        }

        var canonical = ResolveExistingPath(existingAncestor);
        while (missingSegments.TryPop(out var segment))
        {
            canonical = Path.Combine(canonical, segment);
        }

        return TrimTrailingSeparators(canonical);
    }

    internal static string NormalizeFinalPath(string finalPath)
    {
        const string uncPrefix = @"\\?\UNC\";
        const string devicePrefix = @"\\?\";

        if (finalPath.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
        {
            return @"\\" + finalPath[uncPrefix.Length..];
        }

        return finalPath.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase)
            ? finalPath[devicePrefix.Length..]
            : finalPath;
    }

    private static string ResolveExistingPath(string path)
    {
        using var handle = CreateFileW(
            path,
            0,
            FileShareRead | FileShareWrite | FileShareDelete,
            IntPtr.Zero,
            OpenExisting,
            FileFlagBackupSemantics,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new IOException(
                $"Unable to resolve final path: {path}",
                new Win32Exception(Marshal.GetLastWin32Error()));
        }

        var capacity = 512;
        while (true)
        {
            var buffer = new StringBuilder(capacity);
            var length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
            if (length == 0)
            {
                throw new IOException(
                    $"Unable to resolve final path: {path}",
                    new Win32Exception(Marshal.GetLastWin32Error()));
            }

            if (length < buffer.Capacity)
            {
                return NormalizeFinalPath(buffer.ToString());
            }

            capacity = checked((int)length + 1);
        }
    }

    private static string TrimTrailingSeparators(string path)
    {
        var root = Path.GetPathRoot(path);
        return root is not null && path.Equals(root, StringComparison.OrdinalIgnoreCase)
            ? path
            : path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
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

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file,
        StringBuilder filePath,
        uint filePathLength,
        uint flags);
}
