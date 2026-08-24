using System.Text;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Launcher.Logging;

public sealed class FileExternalLogger : IExternalLogger
{
    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;
    private readonly SemaphoreSlim _writeLock = new(1, 1);

    public FileExternalLogger(LocalDataLayout layout, WriteBoundary boundary)
    {
        _layout = layout;
        _boundary = boundary;
    }

    public async Task LogExceptionAsync(
        Exception exception,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(exception);
        var logPath = Path.Combine(_layout.Logs, "launcher.log");
        _boundary.EnsureAllowed(_layout.Logs);
        _boundary.EnsureAllowed(logPath);

        await _writeLock.WaitAsync(cancellationToken);
        try
        {
            Directory.CreateDirectory(_layout.Logs);
            var text = new StringBuilder()
                .Append('[').Append(DateTimeOffset.UtcNow.ToString("O")).AppendLine("]")
                .AppendLine(exception.ToString())
                .ToString();
            await File.AppendAllTextAsync(logPath, text, cancellationToken);
        }
        finally
        {
            _writeLock.Release();
        }
    }
}
