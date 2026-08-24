namespace DSRRandomizer.Launcher.Logging;

public interface IExternalLogger
{
    Task LogExceptionAsync(Exception exception, CancellationToken cancellationToken);
}
