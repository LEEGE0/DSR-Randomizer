using System.Text.Json;
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

        if (args is ["--launch"])
        {
            await WriteJsonAsync(new
            {
                success = false,
                error = "Game launch is unsupported until dedicated-save and online-blocking safety is installed."
            });
            return 2;
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
            error = "Invalid arguments. Supported commands: --verify <game-path>, --initialize-runtime <game-path>, --status."
        });
        return 2;
    }

    private Task WriteJsonAsync<T>(T value) =>
        _output.WriteLineAsync(JsonSerializer.Serialize(value));
}
