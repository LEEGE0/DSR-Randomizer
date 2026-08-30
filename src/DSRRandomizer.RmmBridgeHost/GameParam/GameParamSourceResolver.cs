using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using DSRRandomizer.Foundation.Paths;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.RmmBridgeHost.GameParam;

public sealed record GameParamSourceSet(
    string ExternalRoot,
    string RuntimeRoot,
    string EnemyRandomizerRoot,
    string BasePath,
    string RandomizedPath,
    string DefinitionsDirectory,
    IReadOnlyList<string> DefinitionPaths,
    string SourceInstallationRoot,
    string OverhaulPath,
    string OutputDirectory,
    string OutputPath,
    string ManifestPath);

public sealed partial class GameParamSourceResolver(IPathCanonicalizer canonicalizer)
{
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint FileShareDelete = 0x00000004;
    private const uint OpenExisting = 3;
    private const uint OpenReparsePoint = 0x00200000;

    public GameParamSourceSet Resolve(
        string externalRoot,
        string runtimeId,
        string sourceInstallationRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        ArgumentException.ThrowIfNullOrWhiteSpace(runtimeId);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceInstallationRoot);
        if (!RuntimeIdPattern().IsMatch(runtimeId))
            throw new ArgumentException("The active runtime ID is invalid.", nameof(runtimeId));

        string canonicalExternal = RequireCanonicalRoot(externalRoot, "external");
        string canonicalSource = RequireCanonicalRoot(sourceInstallationRoot, "source installation");
        string runtimeRoot = Path.GetFullPath(Path.Combine(canonicalExternal, "runtimes", runtimeId));
        RequireContained(runtimeRoot, canonicalExternal, "runtime");
        runtimeRoot = RequireExistingDirectory(runtimeRoot, canonicalExternal, "runtime");
        string modsRoot = RequireExistingDirectory(
            Path.Combine(runtimeRoot, "Mods"), runtimeRoot, "runtime Mods");

        List<string> candidates = EnumerateCandidates(modsRoot);
        if (candidates.Count != 1)
        {
            throw new InvalidDataException(
                $"Expected exactly one DS1EnemyRandomizer directory below the active runtime Mods directory; found {candidates.Count}.");
        }

        string enemyRoot = RequireExistingDirectory(
            candidates[0], runtimeRoot, "enemy-randomizer");
        string basePath = RequirePrivateFile(
            Path.Combine(enemyRoot, @"dist1\Vanilla\GameParam.parambnd.dcx"),
            enemyRoot,
            "base GameParam");
        string randomPath = RequirePrivateFile(
            Path.Combine(enemyRoot, @"param\GameParam\GameParam.parambnd.dcx"),
            enemyRoot,
            "randomized GameParam");
        string defs = RequireExistingDirectory(
            Path.Combine(enemyRoot, @"dist1\Defs"), enemyRoot, "PARAMDEF");
        string[] definitionPaths = Directory.EnumerateFiles(defs, "*.xml", SearchOption.TopDirectoryOnly)
            .OrderBy(static path => Path.GetFileName(path), StringComparer.OrdinalIgnoreCase)
            .Select(path => RequirePrivateFile(path, defs, "PARAMDEF"))
            .ToArray();
        if (definitionPaths.Length == 0)
            throw new InvalidDataException("The enemy-randomizer PARAMDEF directory is empty.");

        string overhaulPath = RequirePrivateFile(
            Path.Combine(canonicalSource, @"overhaul\GameParam.parambnd.dcx"),
            canonicalSource,
            "Steam Overhaul GameParam");

        string outputDirectory = Path.Combine(
            canonicalExternal, "components", "rmm-bridge", "content", "overhaul");
        string canonicalOutputDirectory = canonicalizer.Canonicalize(outputDirectory);
        RequireContained(canonicalOutputDirectory, canonicalExternal, "output");
        EnsureExistingSegmentsAreNotReparse(canonicalExternal, outputDirectory, "output");
        string outputPath = Path.Combine(canonicalOutputDirectory, "GameParam.parambnd.dcx");
        string manifestPath = Path.Combine(canonicalOutputDirectory, "gameparam-merge-manifest.json");
        RequireContained(canonicalizer.Canonicalize(outputPath), canonicalExternal, "output");
        RequireContained(canonicalizer.Canonicalize(manifestPath), canonicalExternal, "manifest");

        return new GameParamSourceSet(
            canonicalExternal,
            runtimeRoot,
            enemyRoot,
            basePath,
            randomPath,
            defs,
            definitionPaths,
            canonicalSource,
            overhaulPath,
            canonicalOutputDirectory,
            outputPath,
            manifestPath);
    }

    private List<string> EnumerateCandidates(string modsRoot)
    {
        var candidates = new List<string>();
        foreach (string firstLevel in Directory.EnumerateDirectories(modsRoot, "*", SearchOption.TopDirectoryOnly))
        {
            RejectReparse(firstLevel, "runtime mod directory");
            if (Path.GetFileName(firstLevel).Equals("DS1EnemyRandomizer", StringComparison.Ordinal))
                candidates.Add(firstLevel);

            foreach (string secondLevel in Directory.EnumerateDirectories(firstLevel, "*", SearchOption.TopDirectoryOnly))
            {
                RejectReparse(secondLevel, "runtime mod directory");
                if (Path.GetFileName(secondLevel).Equals("DS1EnemyRandomizer", StringComparison.Ordinal))
                    candidates.Add(secondLevel);
            }
        }
        return candidates;
    }

    private string RequireCanonicalRoot(string path, string description)
    {
        if (!Path.IsPathFullyQualified(path) || path.StartsWith(@"\\", StringComparison.Ordinal))
            throw new ArgumentException($"The {description} root must be an absolute local path.");
        string full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        string canonical = canonicalizer.Canonicalize(full);
        if (!canonical.Equals(full, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"The {description} root is not canonical.");
        if (!Directory.Exists(canonical))
            throw new DirectoryNotFoundException($"The {description} root is missing: {canonical}");
        RejectReparse(canonical, $"{description} root");
        return canonical;
    }

    private string RequireExistingDirectory(string path, string root, string description)
    {
        if (!Directory.Exists(path))
            throw new InvalidDataException($"The required {description} directory is missing: {path}");
        EnsureExistingSegmentsAreNotReparse(root, path, description);
        string canonical = canonicalizer.Canonicalize(path);
        RequireContained(canonical, root, description);
        return canonical;
    }

    private string RequirePrivateFile(string path, string root, string description)
    {
        if (!File.Exists(path))
            throw new InvalidDataException($"The required {description} file is missing: {path}");
        EnsureExistingSegmentsAreNotReparse(root, path, description);
        string canonical = canonicalizer.Canonicalize(path);
        RequireContained(canonical, root, description);

        using SafeFileHandle handle = CreateFileW(
            path,
            0,
            FileShareRead | FileShareWrite | FileShareDelete,
            IntPtr.Zero,
            OpenExisting,
            OpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new IOException(
                $"Unable to inspect {description}: {path}",
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
        if (!GetFileInformationByHandle(handle, out ByHandleFileInformation information))
        {
            throw new IOException(
                $"Unable to inspect {description}: {path}",
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
        var attributes = (FileAttributes)information.FileAttributes;
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0
            || information.NumberOfLinks != 1)
        {
            throw new IOException(
                $"The {description} must be a regular non-reparse file with exactly one hard link: {path}");
        }
        return canonical;
    }

    private static void EnsureExistingSegmentsAreNotReparse(
        string root,
        string path,
        string description)
    {
        RequireContained(path, root, description);
        string relative = Path.GetRelativePath(root, path);
        string current = root;
        RejectReparse(current, description);
        foreach (string segment in relative.Split(
                     [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                     StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (File.Exists(current) || Directory.Exists(current))
                RejectReparse(current, description);
            else
                break;
        }
    }

    private static void RejectReparse(string path, string description)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException($"Reparse points are not allowed in the {description} path: {path}");
    }

    private static void RequireContained(string candidate, string root, string description)
    {
        string normalizedRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        string normalizedCandidate = Path.TrimEndingDirectorySeparator(Path.GetFullPath(candidate));
        if (!normalizedCandidate.Equals(normalizedRoot, StringComparison.OrdinalIgnoreCase)
            && !normalizedCandidate.StartsWith(
                normalizedRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new UnauthorizedAccessException(
                $"The {description} path escapes its required root: {normalizedCandidate}");
        }
    }

    [GeneratedRegex("^runtime-[0-9a-f]{8,128}$", RegexOptions.CultureInvariant | RegexOptions.NonBacktracking)]
    private static partial Regex RuntimeIdPattern();

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
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
}
