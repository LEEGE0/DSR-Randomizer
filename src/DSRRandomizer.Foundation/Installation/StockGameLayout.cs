using System.Collections.Immutable;

namespace DSRRandomizer.Foundation.Installation;

public static class StockGameLayout
{
    public static readonly ImmutableHashSet<string> RootFiles =
        ImmutableHashSet.Create(
            StringComparer.OrdinalIgnoreCase,
            "DarkSoulsRemastered.exe",
            "steam_api64.dll",
            "binkw64.dll",
            "fmod_event_net64.dll",
            "fmod_event64.dll",
            "fmodex64.dll",
            "xinput1_3.dll");

    public static readonly ImmutableHashSet<string> DataDirectories =
        ImmutableHashSet.Create(
            StringComparer.OrdinalIgnoreCase,
            "chr", "event", "facegen", "font", "map", "menu", "movww", "msg",
            "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
            "sfx", "shader", "sound");
}
