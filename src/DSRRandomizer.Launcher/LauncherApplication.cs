using System.IO;
using System.Text.Json;
using DSRRandomizer.Foundation.Packaging;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher;

public sealed class LauncherApplication
{
    private readonly ILauncherService _service;
    private readonly TextWriter _output;
    private readonly TextWriter _error;

    public LauncherApplication(
        ILauncherService service,
        TextWriter output,
        TextWriter error)
    {
        _service = service;
        _output = output;
        _error = error;
    }

    public async Task<int> RunAsync(string[] args, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(args);

        if (args is ["--validate-package", var packagePath])
        {
            if (!Directory.Exists(packagePath))
            {
                await WriteJsonAsync(new
                {
                    success = false,
                    prohibitedPaths = new[] { packagePath },
                    error = "Package directory does not exist."
                });
                return 6;
            }

            var paths = EnumeratePackagePaths(packagePath);
            var prohibited = new ReleaseContentGuard().Validate(paths);
            await WriteJsonAsync(new
            {
                success = prohibited.Count == 0,
                prohibitedPaths = prohibited
            });
            return prohibited.Count == 0 ? 0 : 6;
        }

        if (args is ["--launch"])
        {
            await WriteJsonAsync(new
            {
                success = false,
                error = "Game launch is unsupported until dedicated-save and online-blocking safety is installed."
            });
            return 2;
        }

        if (args is ["--prepare-save", var steamId])
        {
            if (steamId.Length is < 16 or > 20 || !steamId.All(char.IsAsciiDigit))
            {
                await WriteJsonAsync(new
                {
                    success = false,
                    error = "SteamID must contain exactly 16 to 20 decimal digits."
                });
                return 2;
            }

            try
            {
                var result = await _service.PrepareDedicatedSaveAsync(
                    steamId,
                    firstCopyConfirmed: false,
                    cancellationToken);
                await WriteJsonAsync(new
                {
                    success = result.Ready,
                    reusedExisting = result.ReusedExisting,
                    savePath = result.SavePath,
                    errorCode = result.ErrorCode,
                    error = result.Message
                });
                return result.Ready ? 0 : 7;
            }
            catch (Exception exception) when (
                exception is IOException
                    or UnauthorizedAccessException
                    or ArgumentException
                    or JsonException)
            {
                await _error.WriteLineAsync(exception.Message);
                await WriteJsonAsync(new { success = false, error = exception.Message });
                return 7;
            }
        }

        if (args is ["--verify", var gamePath])
        {
            var result = await _service.VerifyAsync(gamePath, cancellationToken);
            await WriteJsonAsync(result);
            return result.IsValid ? 0 : 3;
        }

        if (args is ["--initialize-runtime", var installationPath])
        {
            try
            {
                var verification = await _service.VerifyAsync(
                    installationPath,
                    cancellationToken);
                if (!verification.IsValid)
                {
                    await WriteJsonAsync(verification);
                    return 3;
                }

                var manifest = await _service.InitializeRuntimeAsync(
                    installationPath,
                    progress: null,
                    cancellationToken);
                await WriteJsonAsync(new
                {
                    success = true,
                    manifest.RuntimeId,
                    manifest.RuntimePath
                });
                return 0;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception exception) when (
                exception is IOException
                    or UnauthorizedAccessException
                    or ArgumentException)
            {
                await _error.WriteLineAsync(exception.Message);
                await WriteJsonAsync(new { success = false, error = exception.Message });
                return 4;
            }
        }

        if (args is ["--status"])
        {
            var readiness = await _service.GetReadinessAsync(cancellationToken);
            await WriteJsonAsync(readiness);
            return readiness.IsReady ? 0 : 5;
        }

        await WriteJsonAsync(new
        {
            success = false,
            error = "Invalid arguments. Supported commands: --verify <game-path>, --initialize-runtime <game-path>, --prepare-save <SteamID>, --status."
        });
        return 2;
    }

    private Task WriteJsonAsync<T>(T value) =>
        _output.WriteLineAsync(JsonSerializer.Serialize(value));

    private static IReadOnlyList<string> EnumeratePackagePaths(string packageRoot)
    {
        var root = Path.GetFullPath(packageRoot);
        var paths = new List<string>();
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.TryPop(out var directory))
        {
            foreach (var entry in new DirectoryInfo(directory).EnumerateFileSystemInfos())
            {
                var relativePath = Path.GetRelativePath(root, entry.FullName)
                    .Replace(Path.DirectorySeparatorChar, '/');
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                {
                    paths.Add($"reparse-point:{relativePath}");
                }
                else if ((entry.Attributes & FileAttributes.Directory) != 0)
                {
                    pending.Push(entry.FullName);
                }
                else
                {
                    paths.Add(relativePath);
                }
            }
        }

        paths.Sort(StringComparer.Ordinal);
        return paths;
    }
}
