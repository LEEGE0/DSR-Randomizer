using System.Diagnostics;
using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.RmmBridgeHost;

public sealed class WindowsBridgeHostPlatform : IBridgeHostPlatform
{
    private readonly WindowsPathCanonicalizer _canonicalizer = new();

    public BridgeBindingResult ValidateBinding(BridgeHostArguments arguments)
    {
        try
        {
            var externalRoot = _canonicalizer.Canonicalize(arguments.ExternalRoot);
            if (!externalRoot.Equals(arguments.ExternalRoot, StringComparison.OrdinalIgnoreCase))
            {
                return BridgeBindingResult.Failure("The external root is not canonical.");
            }

            using var process = Process.GetProcessById(checked((int)arguments.GamePid));
            var imagePath = process.MainModule?.FileName
                ?? throw new InvalidOperationException("The game image path is unavailable.");
            var expectedImage = Path.Combine(
                externalRoot, "runtimes", arguments.RuntimeId, "DarkSoulsRemastered.exe");
            if (!_canonicalizer.Canonicalize(imagePath).Equals(
                    _canonicalizer.Canonicalize(expectedImage),
                    StringComparison.OrdinalIgnoreCase))
            {
                return BridgeBindingResult.Failure("The live process is not the selected copied runtime.");
            }

            using var runtimeDocument = ReadStrictObject(
                Path.Combine(externalRoot, "runtime-current.json"),
                ["runtimeId", "relativeRuntimePath", "manifestSha256"]);
            var runtimeRoot = Path.GetFullPath(Path.Combine(
                externalRoot,
                runtimeDocument.RootElement.GetProperty("relativeRuntimePath").GetString()
                    ?? throw new InvalidDataException("The runtime path is missing.")));
            if (!runtimeDocument.RootElement.GetProperty("runtimeId").GetString()!
                    .Equals(arguments.RuntimeId, StringComparison.Ordinal)
                || !_canonicalizer.Canonicalize(runtimeRoot).Equals(
                    Path.GetDirectoryName(_canonicalizer.Canonicalize(expectedImage)),
                    StringComparison.OrdinalIgnoreCase))
            {
                return BridgeBindingResult.Failure("The runtime pointer does not match the game process.");
            }

            using var selectionDocument = ReadStrictObject(
                Path.Combine(externalRoot, "config", "selected-save-profile.json"),
                ["steamId", "sourcePath"]);
            if (!selectionDocument.RootElement.GetProperty("steamId").GetString()!
                    .Equals(arguments.SteamId, StringComparison.Ordinal))
            {
                return BridgeBindingResult.Failure("The selected save profile changed.");
            }

            return BridgeBindingResult.Success();
        }
        catch (Exception exception) when (
            exception is ArgumentException or InvalidOperationException or IOException
                or UnauthorizedAccessException or JsonException)
        {
            return BridgeBindingResult.Failure(exception.Message);
        }
    }

    public void SignalReady(string eventName)
    {
        using var readyEvent = EventWaitHandle.OpenExisting(eventName);
        if (!readyEvent.Set())
        {
            throw new InvalidOperationException("The bridge ready event could not be signaled.");
        }
    }

    public async Task<uint> WaitForExitAsync(
        uint processId,
        CancellationToken cancellationToken)
    {
        using var process = Process.GetProcessById(checked((int)processId));
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        return unchecked((uint)process.ExitCode);
    }

    private static JsonDocument ReadStrictObject(
        string path,
        IReadOnlyCollection<string> expectedNames)
    {
        var information = new FileInfo(path);
        if (!information.Exists || information.Length > 65_536)
        {
            throw new InvalidDataException($"A bridge configuration file is missing or oversized: {path}");
        }
        var document = JsonDocument.Parse(File.ReadAllBytes(path), new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 8
        });
        try
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidDataException("Bridge configuration must be a JSON object.");
            }
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in document.RootElement.EnumerateObject())
            {
                if (!names.Add(property.Name))
                {
                    throw new InvalidDataException($"Duplicate JSON property: {property.Name}");
                }
            }
            if (!names.SetEquals(expectedNames))
            {
                throw new InvalidDataException("Bridge configuration properties do not match the schema.");
            }
            return document;
        }
        catch
        {
            document.Dispose();
            throw;
        }
    }
}
