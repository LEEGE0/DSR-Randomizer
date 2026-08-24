using System.Runtime.InteropServices;

namespace DSRRandomizer.Foundation.Saves;

public sealed class WindowsKnownFolderProvider : IKnownFolderProvider
{
    private static readonly Guid DocumentsFolderId = new("FDD39AD0-238F-46AF-ADB4-6C85480369C7");

    public string GetDocumentsPath()
    {
        var documentsFolderId = DocumentsFolderId;
        var result = SHGetKnownFolderPath(ref documentsFolderId, 0, IntPtr.Zero, out var path);
        Marshal.ThrowExceptionForHR(result);

        try
        {
            return Marshal.PtrToStringUni(path)
                ?? throw new InvalidOperationException("The Documents known folder path was empty.");
        }
        finally
        {
            Marshal.FreeCoTaskMem(path);
        }
    }

    [DllImport("shell32.dll", ExactSpelling = true)]
    private static extern int SHGetKnownFolderPath(
        [In] ref Guid knownFolderId,
        uint flags,
        IntPtr token,
        out IntPtr path);
}
