namespace DSRRandomizer.Launcher.Safety;

public static class DedicatedSaveProtection
{
    public const ulong RequiredFlags = (1UL << 0) | (1UL << 9);

    public static bool IsExact(ulong flags) => flags == RequiredFlags;
}
