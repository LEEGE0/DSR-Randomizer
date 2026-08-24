using System.Collections.ObjectModel;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.ViewModels;

public sealed class MainWindowViewModel : ObservableObject
{
    private readonly ILauncherService _service;
    private readonly IExternalLogger _logger;
    private readonly string _localDataRoot;
    private string _gamePath = string.Empty;
    private string _status = "Select the Dark Souls Remastered installation to begin.";
    private double _progressPercent;
    private bool _isBusy;
    private bool _saveProfilesDiscovered;
    private SaveProfileCandidate? _selectedSaveProfile;
    private string _selectedSaveSourcePath = string.Empty;
    private string _dedicatedSavePath = string.Empty;
    private bool _firstCopyConfirmed;

    public MainWindowViewModel(
        ILauncherService service,
        IExternalLogger logger,
        string localDataRoot)
    {
        ArgumentNullException.ThrowIfNull(service);
        ArgumentNullException.ThrowIfNull(logger);
        ArgumentException.ThrowIfNullOrWhiteSpace(localDataRoot);
        _service = service;
        _logger = logger;
        _localDataRoot = Path.GetFullPath(localDataRoot);
        VerifyCommand = new AsyncRelayCommand(
            _ => VerifyAsync(),
            _ => CanMutate());
        InitializeCommand = new AsyncRelayCommand(
            _ => InitializeAsync(),
            _ => CanMutate());
        PrepareSaveCommand = new AsyncRelayCommand(
            _ => PrepareSaveAsync(),
            _ => !IsBusy);
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

    public ObservableCollection<SaveProfileCandidate> SaveProfiles { get; } = [];

    public SaveProfileCandidate? SelectedSaveProfile
    {
        get => _selectedSaveProfile;
        set
        {
            if (SetProperty(ref _selectedSaveProfile, value))
            {
                FirstCopyConfirmed = false;
                SelectedSaveSourcePath = value?.SourcePath ?? string.Empty;
                DedicatedSavePath = value is null
                    ? string.Empty
                    : Path.Combine(
                        _localDataRoot,
                        "saves",
                        value.SteamId,
                        "DRAKS0005.rmm");
            }
        }
    }

    public string SelectedSaveSourcePath
    {
        get => _selectedSaveSourcePath;
        private set => SetProperty(ref _selectedSaveSourcePath, value);
    }

    public string DedicatedSavePath
    {
        get => _dedicatedSavePath;
        private set => SetProperty(ref _dedicatedSavePath, value);
    }

    public bool FirstCopyConfirmed
    {
        get => _firstCopyConfirmed;
        set => SetProperty(ref _firstCopyConfirmed, value);
    }

    public AsyncRelayCommand VerifyCommand { get; }

    public AsyncRelayCommand InitializeCommand { get; }

    public AsyncRelayCommand PrepareSaveCommand { get; }

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

    private async Task PrepareSaveAsync()
    {
        IsBusy = true;
        try
        {
            if (!_saveProfilesDiscovered)
            {
                var profiles = await _service.DiscoverSaveProfilesAsync(CancellationToken.None);
                foreach (var profile in profiles)
                {
                    SaveProfiles.Add(profile);
                }

                _saveProfilesDiscovered = true;
                if (SaveProfiles.Count == 1)
                {
                    SelectedSaveProfile = SaveProfiles[0];
                }
            }

            if (SaveProfiles.Count == 0)
            {
                Status = "No Dark Souls Remastered save profile was discovered.";
                return;
            }

            if (SelectedSaveProfile is null)
            {
                Status = "Multiple save profiles were discovered. Select the exact SteamID to continue.";
                return;
            }

            var result = await _service.PrepareDedicatedSaveAsync(
                SelectedSaveProfile.SteamId,
                FirstCopyConfirmed,
                CancellationToken.None);
            if (result.Ready)
            {
                Status = result.ReusedExisting
                    ? $"Reusing existing DRAKS0005.rmm at {result.SavePath}. Launch remains locked."
                    : $"Dedicated DRAKS0005.rmm created at {result.SavePath}. Launch remains locked.";
                return;
            }

            Status = !FirstCopyConfirmed
                && result.ErrorCode == SaveErrorCode.FirstCopyConfirmationRequired
                ? $"First-copy confirmation required. Source: {SelectedSaveSourcePath} Destination: {DedicatedSavePath}"
                : $"Dedicated save preparation failed: {result.Message}";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"Dedicated save preparation failed: {exception.Message}";
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
        PrepareSaveCommand.RaiseCanExecuteChanged();
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
