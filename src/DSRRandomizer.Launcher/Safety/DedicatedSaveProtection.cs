namespace DSRRandomizer.Launcher.Safety;

public static class DedicatedSaveProtection
{
    public const ulong RequiredFlags = (1UL << 2) - 1UL;

    public static bool IsExact(ulong flags) => flags == RequiredFlags;
}
