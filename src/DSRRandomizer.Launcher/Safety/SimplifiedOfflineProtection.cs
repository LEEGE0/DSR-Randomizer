namespace DSRRandomizer.Launcher.Safety;

public static class SimplifiedOfflineProtection
{
    public const ulong RequiredFlags = (1UL << 7) - 1UL;

    public static bool IsExact(ulong flags) => flags == RequiredFlags;
}
