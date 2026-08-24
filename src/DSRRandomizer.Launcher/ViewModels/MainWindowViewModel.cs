using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.ViewModels;

public sealed class MainWindowViewModel : ObservableObject
{
    private readonly ILauncherService _service;
    private readonly IExternalLogger _logger;
    private string _gamePath = string.Empty;
    private string _status = "Select the Dark Souls Remastered installation to begin.";
    private double _progressPercent;
    private bool _isBusy;

    public MainWindowViewModel(
        ILauncherService service,
        IExternalLogger logger)
    {
        _service = service;
        _logger = logger;
        VerifyCommand = new AsyncRelayCommand(
            _ => VerifyAsync(),
            _ => CanMutate());
        InitializeCommand = new AsyncRelayCommand(
            _ => InitializeAsync(),
            _ => CanMutate());
    }

    public string GamePath
    {
        get => _gamePath;
        set
        {
            if (SetProperty(ref _gamePath, value))
            {
                RaiseCommandStates();
            }
        }
    }

    public string Status
    {
        get => _status;
        private set => SetProperty(ref _status, value);
    }

    public double ProgressPercent
    {
        get => _progressPercent;
        private set => SetProperty(ref _progressPercent, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                RaiseCommandStates();
            }
        }
    }

    public bool CanLaunch => false;

    public AsyncRelayCommand VerifyCommand { get; }

    public AsyncRelayCommand InitializeCommand { get; }

    private async Task VerifyAsync()
    {
        IsBusy = true;
        try
        {
            var result = await _service.VerifyAsync(GamePath, CancellationToken.None);
            Status = result.IsValid
                ? "Installation verified. The original game and installed Overhaul remain read-only."
                : $"Installation verification failed: {string.Join("; ", result.Errors)}";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"Installation verification failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task InitializeAsync()
    {
        IsBusy = true;
        ProgressPercent = 0;
        try
        {
            var verification = await _service.VerifyAsync(GamePath, CancellationToken.None);
            if (!verification.IsValid)
            {
                Status = $"Installation verification failed: {string.Join("; ", verification.Errors)}";
                return;
            }

            var progress = new InlineProgress<RuntimeBuildProgress>(update =>
            {
                ProgressPercent = update.TotalBytes <= 0
                    ? 0
                    : Math.Clamp(update.CopiedBytes * 100d / update.TotalBytes, 0, 100);
                Status = $"Creating external runtime: {update.RelativePath}";
            });
            await _service.InitializeRuntimeAsync(
                GamePath,
                progress,
                CancellationToken.None);
            var readiness = await _service.GetReadinessAsync(CancellationToken.None);
            if (!readiness.IsReady)
            {
                throw new IOException(string.Join("; ", readiness.Errors));
            }

            ProgressPercent = 100;
            Status = "External runtime is ready. Launch stays locked until dedicated-save and online-blocking safety is installed.";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"Runtime creation failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool CanMutate() => !IsBusy && !string.IsNullOrWhiteSpace(GamePath);

    private void RaiseCommandStates()
    {
        VerifyCommand.RaiseCanExecuteChanged();
        InitializeCommand.RaiseCanExecuteChanged();
    }

    private async Task LogWithoutMaskingAsync(Exception exception)
    {
        try
        {
            await _logger.LogExceptionAsync(exception, CancellationToken.None);
        }
        catch
        {
            // A logging failure must not hide the operation's original error.
        }
    }

    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
