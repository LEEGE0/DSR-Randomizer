using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Launcher.Native;

internal sealed class SafeProcessHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal SafeProcessHandle(IntPtr handle)
        : base(ownsHandle: true)
    {
        SetHandle(handle);
    }

    protected override bool ReleaseHandle() => NativeMethods.CloseHandle(handle);
}
