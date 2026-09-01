using System.IO;
using System.Text.Json;
using DSRRandomizer.Foundation.Packaging;
using DSRRandomizer.Launcher.Safety;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher;

public sealed class LauncherApplication
{
    private readonly ILauncherService? _service;
    private readonly TextWriter _output;
    private readonly TextWriter _error;
    private readonly bool _externalRootSelected;

    public LauncherApplication(
        ILauncherService? service,
        TextWriter output,
        TextWriter error,
        bool externalRootSelected = true)
    {
        _service = service;
        _output = output;
        _error = error;
        _externalRootSelected = externalRootSelected;
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
            if (prohibited.Count == 0)
            {
                prohibited = ReleaseArtifactIdentityValidator.Validate(packagePath);
            }
            await WriteJsonAsync(new
            {
                success = prohibited.Count == 0,
                prohibitedPaths = prohibited
            });
            return prohibited.Count == 0 ? 0 : 6;
        }

        if (!_externalRootSelected)
        {
            await WriteJsonAsync(new
            {
                success = false,
                errorCode = "EXTERNAL_ROOT_NOT_SELECTED",
                error = "Select an external material root with --set-root before running this command."
            });
            return 8;
        }

        if (args is ["--launch", var launchSteamId])
        {
            if (!IsSteamId(launchSteamId))
            {
                await WriteJsonAsync(new
                {
                    success = false,
                    error = "SteamID must contain 1 to 20 decimal digits."
                });
                return 2;
            }

            var result = await Service.LaunchModdedAsync(
                launchSteamId,
                cancellationToken);
            await WriteJsonAsync(new
            {
                success = result.Started,
                errorCode = result.ErrorCode,
                exitCode = result.ExitCode
            });
            return result.Started ? 0 : 9;
        }

        if (args is ["--prepare-save", var steamId])
        {
            if (!IsSteamId(steamId))
            {
                await WriteJsonAsync(new
                {
                    success = false,
                    error = "SteamID must contain 1 to 20 decimal digits."
                });
                return 2;
            }

            try
            {
                var result = await Service.PrepareDedicatedSaveAsync(
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
            var result = await Service.VerifyAsync(gamePath, cancellationToken);
            await WriteJsonAsync(result);
            return result.IsValid ? 0 : 3;
        }

        if (args is ["--initialize-runtime", var installationPath])
        {
            try
            {
                var verification = await Service.VerifyAsync(
                    installationPath,
                    cancellationToken);
                if (!verification.IsValid)
                {
                    await WriteJsonAsync(verification);
                    return 3;
                }

                var manifest = await Service.InitializeRuntimeAsync(
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
            var readiness = await Service.GetReadinessAsync(cancellationToken);
            await WriteJsonAsync(readiness);
            return readiness.IsReady ? 0 : 5;
        }

        await WriteJsonAsync(new
        {
            success = false,
            error = "Invalid arguments. Supported commands: --set-root <external-root>, --verify <game-path>, --initialize-runtime <game-path>, --prepare-save <SteamID>, --launch <SteamID>, --status."
        });
        return 2;
    }

    private Task WriteJsonAsync<T>(T value) =>
        _output.WriteLineAsync(JsonSerializer.Serialize(value));

    private ILauncherService Service => _service ?? throw new InvalidOperationException(
        "EXTERNAL_ROOT_NOT_SELECTED");

    private static bool IsSteamId(string value) =>
        value.Length is >= 1 and <= 20 && value.All(char.IsAsciiDigit);

    private static IReadOnlyList<string> EnumeratePackagePaths(string packageRoot)
    {
        var root = Path.GetFullPath(packageRoot);
        var paths = new List<string>();
        var pending = new Stack<(string Directory, string RelativePath)>();
        pending.Push((root, string.Empty));
        while (pending.TryPop(out var current))
        {
            foreach (var entry in new DirectoryInfo(current.Directory).EnumerateFileSystemInfos())
            {
                var relativePath = string.IsNullOrEmpty(current.RelativePath)
                    ? entry.Name
                    : $"{current.RelativePath}/{entry.Name}";
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                {
                    paths.Add($"reparse-point:{relativePath}");
                }
                else if ((entry.Attributes & FileAttributes.Directory) != 0)
                {
                    pending.Push((entry.FullName, relativePath));
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
