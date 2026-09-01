using System.Text;
using System.Text.Json;

namespace DSRRandomizer.Launcher.Services;

internal enum RandomizerToolKind
{
    Item,
    Enemy
}

internal sealed record RandomizerRuntimeTools(
    string RuntimeRoot,
    string GameExecutable,
    string ModEngineGamePathAnchor,
    string RandomizerRoot,
    string ItemExecutable,
    string EnemyExecutable,
    string ModEngineLauncher,
    string ModEngineLibrary,
    string ModEngineConfiguration);

internal static class RandomizerRuntimeIntegration
{
    internal static bool TryCreateBridgedModEngineConfiguration(
        RandomizerRuntimeTools tools,
        string externalRoot,
        byte[] sourceBytes,
        out byte[] configurationBytes)
    {
        ArgumentNullException.ThrowIfNull(tools);
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        ArgumentNullException.ThrowIfNull(sourceBytes);
        configurationBytes = [];

        string source;
        try
        {
            source = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true).GetString(sourceBytes);
        }
        catch (DecoderFallbackException)
        {
            return false;
        }

        if (!TryReadModEngineExternalDlls(source, out var externalDlls)
            || !TryFindArrayAssignment(source, "modengine", "external_dlls", out var dllArray)
            || !TryFindArrayAssignment(source, "extension.mod_loader", "mods", out var modsArray)
            || !TryReadBooleanAssignment(source, "extension.mod_loader", "enabled", out var loaderEnabled)
            || !loaderEnabled
            || !TryParseModEntries(
                source[(modsArray.OpeningBracket + 1)..modsArray.ClosingBracket],
                out var modEntries))
        {
            return false;
        }

        var (bridgePath, heapPatchPath) = GetRequiredModEngineDllPaths(tools, externalRoot);
        var bridgeRoot = Path.GetDirectoryName(bridgePath)!;
        if (!File.Exists(bridgePath) || !File.Exists(heapPatchPath))
        {
            return false;
        }

        string[] canonicalDlls;
        try
        {
            canonicalDlls = externalDlls.Select(Path.GetFullPath).ToArray();
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
        if (!canonicalDlls.Contains(heapPatchPath, StringComparer.OrdinalIgnoreCase)
            || canonicalDlls.Any(path => Path.GetFileName(path).Equals(
                "DarkSoulsItemRandomizer.exe",
                StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        ModLoaderEntry[] canonicalMods;
        try
        {
            canonicalMods = modEntries
                .Select(entry => entry with { Path = Path.GetFullPath(entry.Path) })
                .ToArray();
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
        if (!canonicalMods.Any(entry =>
                entry.Enabled
                && entry.Name.Equals("randomizer", StringComparison.OrdinalIgnoreCase)
                && entry.Path.Equals(tools.RandomizerRoot, StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        var dllEntries = new[] { bridgePath }
            .Concat(canonicalDlls.Where(path => !Path.GetFileName(path).Equals(
                "DSRRandomizer.RmmBridge.dll",
                StringComparison.OrdinalIgnoreCase)))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(path => JsonSerializer.Serialize(path))
            .ToArray();
        var retainedMods = canonicalMods
            .Where(entry =>
                !entry.Name.Equals("mod1", StringComparison.OrdinalIgnoreCase)
                && !Path.GetFileName(entry.Path.TrimEnd(Path.DirectorySeparatorChar))
                    .Equals("rmm-bridge", StringComparison.OrdinalIgnoreCase))
            .Select(entry => entry.Source)
            .Append(
                $"{{ enabled = true, name = \"mod1\", path = {JsonSerializer.Serialize(bridgeRoot)} }}")
            .ToArray();

        var replacements = new[]
        {
            (Array: dllArray, Entries: (IReadOnlyList<string>)dllEntries),
            (Array: modsArray, Entries: (IReadOnlyList<string>)retainedMods)
        };
        var bridged = source;
        foreach (var replacement in replacements.OrderByDescending(item => item.Array.OpeningBracket))
        {
            bridged = ReplaceArrayContents(bridged, replacement.Array, replacement.Entries);
        }

        var candidate = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false).GetBytes(bridged);
        if (!HasRequiredModEngineConfiguration(tools, externalRoot, candidate)
            || !HasRequiredModLoaderConfiguration(tools, externalRoot, bridged))
        {
            return false;
        }

        configurationBytes = candidate;
        return true;
    }

    internal static bool TryResolve(string runtimeRoot, out RandomizerRuntimeTools? tools)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runtimeRoot);
        var modsRoot = Path.Combine(Path.GetFullPath(runtimeRoot), "Mods");
        if (!Directory.Exists(modsRoot)
            || !Directory.EnumerateFiles(
                modsRoot,
                "DS1EnemyRandomizer.exe",
                SearchOption.AllDirectories).Any())
        {
            tools = null;
            return false;
        }

        tools = Resolve(runtimeRoot);
        return true;
    }

    internal static bool HasRequiredModEngineConfiguration(
        RandomizerRuntimeTools tools,
        string externalRoot,
        byte[] configurationBytes)
    {
        ArgumentNullException.ThrowIfNull(tools);
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        ArgumentNullException.ThrowIfNull(configurationBytes);
        string configuration;
        try
        {
            configuration = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true).GetString(configurationBytes);
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        if (!TryReadModEngineExternalDlls(
                configuration,
                out var externalDlls))
        {
            return false;
        }

        var (expectedBridge, expectedHeapPatch) = GetRequiredModEngineDllPaths(
            tools,
            externalRoot);
        if (!File.Exists(expectedBridge) || !File.Exists(expectedHeapPatch))
        {
            return false;
        }

        try
        {
            var canonicalDlls = externalDlls
                .Select(Path.GetFullPath)
                .ToArray();
            return canonicalDlls.Contains(expectedBridge, StringComparer.OrdinalIgnoreCase)
                && canonicalDlls.Contains(expectedHeapPatch, StringComparer.OrdinalIgnoreCase)
                && !canonicalDlls.Any(path => Path.GetFileName(path).Equals(
                    "DarkSoulsItemRandomizer.exe",
                    StringComparison.OrdinalIgnoreCase));
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    internal static bool HasBridgeOnlyModEngineConfiguration(
        string externalRoot,
        byte[] configurationBytes)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        ArgumentNullException.ThrowIfNull(configurationBytes);
        string configuration;
        try
        {
            configuration = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true).GetString(configurationBytes);
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        if (!TryReadModEngineExternalDlls(configuration, out var externalDlls)
            || externalDlls.Count != 1)
        {
            return false;
        }

        var expectedBridge = Path.GetFullPath(Path.Combine(
            externalRoot,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridge.dll"));
        if (!File.Exists(expectedBridge))
        {
            return false;
        }

        try
        {
            return Path.GetFullPath(externalDlls[0]).Equals(
                expectedBridge,
                StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    internal static (string Bridge, string HeapPatch) GetRequiredModEngineDllPaths(
        RandomizerRuntimeTools tools,
        string externalRoot)
    {
        ArgumentNullException.ThrowIfNull(tools);
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        return (
            Path.GetFullPath(Path.Combine(
                externalRoot,
                "components",
                "rmm-bridge",
                "DSRRandomizer.RmmBridge.dll")),
            Path.GetFullPath(Path.Combine(
                tools.RandomizerRoot,
                "dist1",
                "DLL",
                "DS1HeapPatch.dll")));
    }

    internal static RandomizerProcessRequest CreateToolRequest(
        RandomizerRuntimeTools tools,
        RandomizerToolKind kind)
    {
        ArgumentNullException.ThrowIfNull(tools);
        return new RandomizerProcessRequest(
            kind == RandomizerToolKind.Item
                ? tools.ItemExecutable
                : tools.EnemyExecutable,
            kind == RandomizerToolKind.Item
                ? tools.RuntimeRoot
                : tools.RandomizerRoot,
            []);
    }

    internal static RandomizerProcessRequest CreateModEngineRequest(
        RandomizerRuntimeTools tools,
        string configurationPath)
    {
        ArgumentNullException.ThrowIfNull(tools);
        ArgumentException.ThrowIfNullOrWhiteSpace(configurationPath);
        return new RandomizerProcessRequest(
            tools.ModEngineLauncher,
            Path.GetDirectoryName(tools.ModEngineLauncher)!,
            [
                "--launch-target",
                "dsr",
                "--game-path",
                tools.ModEngineGamePathAnchor,
                "--config",
                Path.GetFullPath(configurationPath)
            ]);
    }

    internal static RandomizerRuntimeTools Resolve(string runtimeRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runtimeRoot);
        var canonicalRuntime = Path.GetFullPath(runtimeRoot);
        var gameExecutable = Path.Combine(canonicalRuntime, "DarkSoulsRemastered.exe");
        if (!File.Exists(gameExecutable))
        {
            throw new FileNotFoundException("The copied game executable is missing.", gameExecutable);
        }
        var modEngineGamePathAnchor = RequireFile(
            canonicalRuntime,
            Path.Combine("chr", "c0000.chrbnd.dcx"));

        var modsRoot = Path.Combine(canonicalRuntime, "Mods");
        if (!Directory.Exists(modsRoot))
        {
            throw new DirectoryNotFoundException("The active runtime Mods directory is missing.");
        }

        var candidates = Directory
            .EnumerateFiles(modsRoot, "DS1EnemyRandomizer.exe", SearchOption.AllDirectories)
            .Select(Path.GetDirectoryName)
            .Where(path => !string.IsNullOrWhiteSpace(path))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        if (candidates.Length != 1)
        {
            throw new IOException(
                $"Expected exactly one DS1EnemyRandomizer installation; found {candidates.Length}.");
        }

        var randomizerRoot = Path.GetFullPath(candidates[0]!);
        var itemExecutable = RequireFile(randomizerRoot, "DarkSoulsItemRandomizer.exe");
        var enemyExecutable = RequireFile(randomizerRoot, "DS1EnemyRandomizer.exe");
        var configuration = RequireFile(randomizerRoot, "config_randomizer.toml");
        var modEngineLauncher = RequireFile(
            randomizerRoot,
            Path.Combine("dist1", "ModEngine", "modengine2_launcher.exe"));
        var modEngineLibrary = RequireFile(
            randomizerRoot,
            Path.Combine("dist1", "ModEngine", "modengine2", "bin", "modengine2.dll"));
        return new RandomizerRuntimeTools(
            canonicalRuntime,
            gameExecutable,
            modEngineGamePathAnchor,
            randomizerRoot,
            itemExecutable,
            enemyExecutable,
            modEngineLauncher,
            modEngineLibrary,
            configuration);
    }

    private static string RequireFile(string root, string relativePath)
    {
        var path = Path.GetFullPath(Path.Combine(root, relativePath));
        if (!File.Exists(path))
        {
            throw new FileNotFoundException($"Required randomizer file is missing: {relativePath}", path);
        }
        return path;
    }

    private static bool TryReadModEngineExternalDlls(
        string configuration,
        out IReadOnlyList<string> externalDlls)
    {
        var sectionLines = new List<string>();
        var inModEngine = false;
        foreach (var line in configuration.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n'))
        {
            var uncommented = RemoveTomlComment(line).Trim();
            if (uncommented.StartsWith("[", StringComparison.Ordinal))
            {
                if (inModEngine)
                {
                    break;
                }
                inModEngine = uncommented.Equals("[modengine]", StringComparison.Ordinal);
                continue;
            }
            if (inModEngine)
            {
                sectionLines.Add(line);
            }
        }

        var assignmentIndex = -1;
        var assignmentOffset = -1;
        for (var index = 0; index < sectionLines.Count; index++)
        {
            var line = RemoveTomlComment(sectionLines[index]);
            var equalsIndex = line.IndexOf('=');
            if (equalsIndex < 0
                || !line[..equalsIndex].Trim().Equals(
                    "external_dlls",
                    StringComparison.Ordinal))
            {
                continue;
            }
            if (assignmentIndex >= 0)
            {
                externalDlls = [];
                return false;
            }
            assignmentIndex = index;
            assignmentOffset = equalsIndex + 1;
        }
        if (assignmentIndex < 0)
        {
            externalDlls = [];
            return false;
        }

        var source = new StringBuilder(sectionLines[assignmentIndex][assignmentOffset..]);
        for (var index = assignmentIndex + 1; index < sectionLines.Count; index++)
        {
            source.Append('\n');
            source.Append(sectionLines[index]);
        }
        return TryParseTomlStringArray(source.ToString(), out externalDlls);
    }

    private static bool HasRequiredModLoaderConfiguration(
        RandomizerRuntimeTools tools,
        string externalRoot,
        string configuration)
    {
        if (!TryReadBooleanAssignment(
                configuration,
                "extension.mod_loader",
                "enabled",
                out var loaderEnabled)
            || !loaderEnabled
            || !TryFindArrayAssignment(
                configuration,
                "extension.mod_loader",
                "mods",
                out var modsArray)
            || !TryParseModEntries(
                configuration[(modsArray.OpeningBracket + 1)..modsArray.ClosingBracket],
                out var entries))
        {
            return false;
        }

        var bridgeRoot = Path.GetFullPath(Path.Combine(externalRoot, "components", "rmm-bridge"));
        try
        {
            var canonical = entries
                .Select(entry => entry with { Path = Path.GetFullPath(entry.Path) })
                .ToArray();
            return canonical.Count(entry =>
                    entry.Enabled
                    && entry.Name.Equals("randomizer", StringComparison.OrdinalIgnoreCase)
                    && entry.Path.Equals(tools.RandomizerRoot, StringComparison.OrdinalIgnoreCase)) == 1
                && canonical.Count(entry =>
                    entry.Enabled
                    && entry.Name.Equals("mod1", StringComparison.OrdinalIgnoreCase)
                    && entry.Path.Equals(bridgeRoot, StringComparison.OrdinalIgnoreCase)) == 1
                && canonical.Count(entry =>
                    entry.Name.Equals("mod1", StringComparison.OrdinalIgnoreCase)
                    || Path.GetFileName(entry.Path.TrimEnd(Path.DirectorySeparatorChar))
                        .Equals("rmm-bridge", StringComparison.OrdinalIgnoreCase)) == 1;
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    private static bool TryReadBooleanAssignment(
        string source,
        string sectionName,
        string key,
        out bool value)
    {
        var found = false;
        value = false;
        var currentSection = string.Empty;
        foreach (var line in source.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n'))
        {
            var uncommented = RemoveTomlComment(line).Trim();
            if (uncommented.StartsWith("[", StringComparison.Ordinal)
                && uncommented.EndsWith("]", StringComparison.Ordinal))
            {
                currentSection = uncommented[1..^1].Trim();
                continue;
            }
            if (!currentSection.Equals(sectionName, StringComparison.Ordinal))
            {
                continue;
            }

            var equalsIndex = uncommented.IndexOf('=');
            if (equalsIndex < 0
                || !uncommented[..equalsIndex].Trim().Equals(key, StringComparison.Ordinal))
            {
                continue;
            }
            if (found || !bool.TryParse(uncommented[(equalsIndex + 1)..].Trim(), out value))
            {
                return false;
            }
            found = true;
        }
        return found;
    }

    private sealed record ModLoaderEntry(
        bool Enabled,
        string Name,
        string Path,
        string Source);

    private static bool TryParseModEntries(
        string source,
        out IReadOnlyList<ModLoaderEntry> entries)
    {
        var parsed = new List<ModLoaderEntry>();
        var index = 0;
        while (true)
        {
            SkipTomlTriviaAndCommas(source, ref index);
            if (index >= source.Length)
            {
                entries = parsed;
                return true;
            }
            if (source[index] != '{')
            {
                entries = [];
                return false;
            }

            var start = index++;
            var inString = false;
            var escaped = false;
            while (index < source.Length)
            {
                var character = source[index++];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (inString && character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == '"')
                {
                    inString = !inString;
                    continue;
                }
                if (!inString && character == '}')
                {
                    break;
                }
            }
            if (index > source.Length || source[index - 1] != '}')
            {
                entries = [];
                return false;
            }

            var raw = source[start..index].Trim();
            if (!TryParseModEntry(raw, out var entry))
            {
                entries = [];
                return false;
            }
            parsed.Add(entry!);
        }
    }

    private static bool TryParseModEntry(string source, out ModLoaderEntry? entry)
    {
        bool? enabled = null;
        string? name = null;
        string? path = null;
        var fields = SplitInlineTableFields(source[1..^1]);
        if (fields is null)
        {
            entry = null;
            return false;
        }
        foreach (var field in fields)
        {
            var equalsIndex = field.IndexOf('=');
            if (equalsIndex < 0)
            {
                entry = null;
                return false;
            }
            var key = field[..equalsIndex].Trim();
            var rawValue = field[(equalsIndex + 1)..].Trim();
            try
            {
                switch (key)
                {
                    case "enabled":
                        if (enabled is not null || !bool.TryParse(rawValue, out var parsed))
                        {
                            entry = null;
                            return false;
                        }
                        enabled = parsed;
                        break;
                    case "name":
                        if (name is not null)
                        {
                            entry = null;
                            return false;
                        }
                        name = JsonSerializer.Deserialize<string>(rawValue);
                        break;
                    case "path":
                        if (path is not null)
                        {
                            entry = null;
                            return false;
                        }
                        path = JsonSerializer.Deserialize<string>(rawValue);
                        break;
                }
            }
            catch (JsonException)
            {
                entry = null;
                return false;
            }
        }
        if (enabled is null || string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(path))
        {
            entry = null;
            return false;
        }
        entry = new ModLoaderEntry(enabled.Value, name, path, source);
        return true;
    }

    private static IReadOnlyList<string>? SplitInlineTableFields(string source)
    {
        var fields = new List<string>();
        var start = 0;
        var inString = false;
        var escaped = false;
        for (var index = 0; index < source.Length; index++)
        {
            var character = source[index];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (inString && character == '\\')
            {
                escaped = true;
                continue;
            }
            if (character == '"')
            {
                inString = !inString;
                continue;
            }
            if (!inString && character == ',')
            {
                fields.Add(source[start..index].Trim());
                start = index + 1;
            }
        }
        if (inString)
        {
            return null;
        }
        fields.Add(source[start..].Trim());
        return fields.All(field => field.Length > 0) ? fields : null;
    }

    private static void SkipTomlTriviaAndCommas(string source, ref int index)
    {
        while (index < source.Length)
        {
            if (char.IsWhiteSpace(source[index]) || source[index] == ',')
            {
                index++;
                continue;
            }
            if (source[index] != '#')
            {
                return;
            }
            while (index < source.Length && source[index] != '\n')
            {
                index++;
            }
        }
    }

    private static bool TryFindArrayAssignment(
        string source,
        string sectionName,
        string key,
        out (int OpeningBracket, int ClosingBracket) array)
    {
        var currentSection = string.Empty;
        var lineStart = 0;
        while (lineStart <= source.Length)
        {
            var newline = source.IndexOf('\n', lineStart);
            var lineEnd = newline >= 0 ? newline : source.Length;
            var line = source[lineStart..lineEnd];
            var uncommented = RemoveTomlComment(line);
            var trimmed = uncommented.Trim();
            if (trimmed.StartsWith("[", StringComparison.Ordinal)
                && trimmed.EndsWith("]", StringComparison.Ordinal))
            {
                currentSection = trimmed[1..^1].Trim();
            }
            else if (currentSection.Equals(sectionName, StringComparison.Ordinal))
            {
                var equalsIndex = uncommented.IndexOf('=');
                if (equalsIndex >= 0
                    && uncommented[..equalsIndex].Trim().Equals(key, StringComparison.Ordinal))
                {
                    var index = lineStart + equalsIndex + 1;
                    SkipTomlTrivia(source, ref index);
                    if (index >= source.Length || source[index] != '[')
                    {
                        array = default;
                        return false;
                    }

                    var openingBracket = index++;
                    var depth = 1;
                    var inString = false;
                    var escaped = false;
                    var inComment = false;
                    while (index < source.Length)
                    {
                        var character = source[index];
                        if (inComment)
                        {
                            inComment = character != '\n';
                            index++;
                            continue;
                        }
                        if (escaped)
                        {
                            escaped = false;
                            index++;
                            continue;
                        }
                        if (inString && character == '\\')
                        {
                            escaped = true;
                            index++;
                            continue;
                        }
                        if (character == '"')
                        {
                            inString = !inString;
                            index++;
                            continue;
                        }
                        if (!inString && character == '#')
                        {
                            inComment = true;
                            index++;
                            continue;
                        }
                        if (!inString && character == '[')
                        {
                            depth++;
                        }
                        else if (!inString && character == ']' && --depth == 0)
                        {
                            array = (openingBracket, index);
                            return true;
                        }
                        index++;
                    }

                    array = default;
                    return false;
                }
            }

            if (newline < 0)
            {
                break;
            }
            lineStart = newline + 1;
        }

        array = default;
        return false;
    }

    private static string ReplaceArrayContents(
        string source,
        (int OpeningBracket, int ClosingBracket) array,
        IReadOnlyList<string> entries)
    {
        var newline = source.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var body = entries.Count == 0
            ? string.Empty
            : newline + string.Join("," + newline, entries.Select(entry => "    " + entry)) + "," + newline;
        return source[..(array.OpeningBracket + 1)] + body + source[array.ClosingBracket..];
    }

    private static bool TryParseTomlStringArray(
        string source,
        out IReadOnlyList<string> values)
    {
        var parsed = new List<string>();
        var index = 0;
        SkipTomlTrivia(source, ref index);
        if (index >= source.Length || source[index++] != '[')
        {
            values = [];
            return false;
        }

        var expectValue = true;
        while (true)
        {
            SkipTomlTrivia(source, ref index);
            if (index >= source.Length)
            {
                values = [];
                return false;
            }
            if (source[index] == ']')
            {
                values = parsed;
                return true;
            }
            if (!expectValue || source[index] != '"')
            {
                values = [];
                return false;
            }

            var start = index++;
            var escaped = false;
            while (index < source.Length)
            {
                var character = source[index++];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == '"')
                {
                    break;
                }
            }
            if (index > source.Length || source[index - 1] != '"')
            {
                values = [];
                return false;
            }
            try
            {
                var value = JsonSerializer.Deserialize<string>(source[start..index]);
                if (string.IsNullOrWhiteSpace(value))
                {
                    values = [];
                    return false;
                }
                parsed.Add(value);
            }
            catch (JsonException)
            {
                values = [];
                return false;
            }

            SkipTomlTrivia(source, ref index);
            if (index < source.Length && source[index] == ',')
            {
                index++;
                expectValue = true;
                continue;
            }
            if (index < source.Length && source[index] == ']')
            {
                values = parsed;
                return true;
            }
            values = [];
            return false;
        }
    }

    private static string RemoveTomlComment(string line)
    {
        var inString = false;
        var escaped = false;
        for (var index = 0; index < line.Length; index++)
        {
            var character = line[index];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (inString && character == '\\')
            {
                escaped = true;
                continue;
            }
            if (character == '"')
            {
                inString = !inString;
                continue;
            }
            if (!inString && character == '#')
            {
                return line[..index];
            }
        }
        return line;
    }

    private static void SkipTomlTrivia(string source, ref int index)
    {
        while (index < source.Length)
        {
            if (char.IsWhiteSpace(source[index]))
            {
                index++;
                continue;
            }
            if (source[index] != '#')
            {
                return;
            }
            while (index < source.Length && source[index] != '\n')
            {
                index++;
            }
        }
    }
}
