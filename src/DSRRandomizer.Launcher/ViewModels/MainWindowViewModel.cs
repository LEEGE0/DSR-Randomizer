using System.Collections.ObjectModel;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Configuration;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.ViewModels;

public sealed class MainWindowViewModel : ObservableObject
{
    private readonly ILauncherService? _service;
    private readonly IExternalLogger _logger;
    private readonly string _materialRoot;
    private readonly ExternalRootSelectionStore? _externalRootStore;
    private readonly bool _materialOperationsAvailable;
    private bool _restartRequired;
    private string _gamePath = string.Empty;
    private string _externalRootPath = string.Empty;
    private string _status;
    private double _progressPercent;
    private bool _isBusy;
    private bool _saveProfilesDiscovered;
    private SaveProfileCandidate? _selectedSaveProfile;
    private string _selectedSaveSourcePath = string.Empty;
    private string _dedicatedSavePath = string.Empty;
    private bool _firstCopyConfirmed;
    private bool _runtimeReady;
    private bool _saveReadyForLaunch;

    public MainWindowViewModel(
        ILauncherService? service,
        IExternalLogger logger,
        string materialRoot,
        ExternalRootSelectionStore? externalRootStore = null,
        bool materialOperationsAvailable = true)
    {
        ArgumentNullException.ThrowIfNull(logger);
        ArgumentNullException.ThrowIfNull(materialRoot);
        _service = service;
        _logger = logger;
        _materialRoot = string.IsNullOrWhiteSpace(materialRoot)
            ? string.Empty
            : Path.GetFullPath(materialRoot);
        _externalRootStore = externalRootStore;
        _materialOperationsAvailable = materialOperationsAvailable;
        _externalRootPath = _materialRoot;
        _status = materialOperationsAvailable
            ? "Select the Dark Souls Remastered installation to begin."
            : "EXTERNAL_ROOT_NOT_SELECTED: select and save an external material root, then restart the launcher.";
        VerifyCommand = new AsyncRelayCommand(
            _ => VerifyAsync(),
            _ => CanMutate());
        InitializeCommand = new AsyncRelayCommand(
            _ => InitializeAsync(),
            _ => CanMutate());
        PrepareSaveCommand = new AsyncRelayCommand(
            _ => PrepareSaveAsync(),
            _ => CanPrepareSave());
        LaunchCommand = new AsyncRelayCommand(
            _ => LaunchAsync(),
            _ => CanLaunch);
        SaveExternalRootCommand = new AsyncRelayCommand(
            _ => SaveExternalRootAsync(),
            _ => !IsBusy && _externalRootStore is not null);
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

    public string ExternalRootPath
    {
        get => _externalRootPath;
        set
        {
            if (SetProperty(ref _externalRootPath, value))
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
                OnPropertyChanged(nameof(AreSaveControlsEnabled));
                RaiseCommandStates();
            }
        }
    }

    public bool CanLaunch => CanPrepareSave()
        && _runtimeReady
        && _saveReadyForLaunch
        && SelectedSaveProfile is not null;

    public bool CanInitialize => CanMutate();

    public bool AreSaveControlsEnabled => CanPrepareSave();

    public ObservableCollection<SaveProfileCandidate> SaveProfiles { get; } = [];

    public SaveProfileCandidate? SelectedSaveProfile
    {
        get => _selectedSaveProfile;
        set
        {
            if (SetProperty(ref _selectedSaveProfile, value))
            {
                _saveReadyForLaunch = false;
                FirstCopyConfirmed = false;
                SelectedSaveSourcePath = value?.SourcePath ?? string.Empty;
                DedicatedSavePath = value is null
                    ? string.Empty
                    : Path.Combine(
                        _materialRoot,
                        "saves",
                        value.SteamId,
                        "DRAKS0005.rmm");
                RaiseCommandStates();
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

    public AsyncRelayCommand LaunchCommand { get; }

    public AsyncRelayCommand SaveExternalRootCommand { get; }

    public async Task LoadAsync()
    {
        if (!_materialOperationsAvailable || _service is null || IsBusy)
        {
            return;
        }

        IsBusy = true;
        try
        {
            var readiness = await Service.GetModdedLaunchReadinessAsync(
                CancellationToken.None);
            _runtimeReady = readiness.IsReady;
            if (!_runtimeReady)
            {
                Status = $"Modded runtime is not ready: {string.Join("; ", readiness.Errors)}";
                return;
            }

            await DiscoverProfilesOnceAsync();
            if (SaveProfiles.Count == 1)
            {
                SelectedSaveProfile = SaveProfiles[0];
            }
            if (SelectedSaveProfile is null)
            {
                Status = "Modded runtime is ready. Select the exact SteamID before launch.";
                return;
            }

            var result = await Service.PrepareDedicatedSaveAsync(
                SelectedSaveProfile.SteamId,
                firstCopyConfirmed: false,
                CancellationToken.None);
            _saveReadyForLaunch = result.Ready
                || (result.ErrorCode == SaveErrorCode.FirstCopyConfirmationRequired
                    && !string.IsNullOrWhiteSpace(SelectedSaveProfile.SourcePath));
            Status = _saveReadyForLaunch
                ? "Modded runtime and save input are ready. Put Steam in Offline Mode before launch."
                : $"Modded launch is not ready: {result.Message}";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"Modded launch readiness failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task VerifyAsync()
    {
        IsBusy = true;
        try
        {
            var result = await Service.VerifyAsync(GamePath, CancellationToken.None);
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
            var verification = await Service.VerifyAsync(GamePath, CancellationToken.None);
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
            await Service.InitializeRuntimeAsync(
                GamePath,
                progress,
                CancellationToken.None);
            var readiness = await Service.GetModdedLaunchReadinessAsync(CancellationToken.None);
            if (!readiness.IsReady)
            {
                throw new IOException(string.Join("; ", readiness.Errors));
            }

            _runtimeReady = true;
            ProgressPercent = 100;
            Status = "External mod runtime is ready. Select a SteamID to launch after placing Steam in Offline Mode.";
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
            await DiscoverProfilesOnceAsync();

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

            var selection = SelectedSaveProfile;
            var firstCopyConfirmed = FirstCopyConfirmed;
            var selectedSaveSourcePath = SelectedSaveSourcePath;
            var dedicatedSavePath = DedicatedSavePath;
            var result = await Service.PrepareDedicatedSaveAsync(
                selection.SteamId,
                firstCopyConfirmed,
                CancellationToken.None);
            if (result.Ready)
            {
                _saveReadyForLaunch = true;
                Status = result.ReusedExisting
                    ? $"Reusing existing DRAKS0005.rmm at {result.SavePath}."
                    : $"Dedicated DRAKS0005.rmm created at {result.SavePath}.";
                return;
            }

            _saveReadyForLaunch = !firstCopyConfirmed
                && result.ErrorCode == SaveErrorCode.FirstCopyConfirmationRequired
                && !string.IsNullOrWhiteSpace(selectedSaveSourcePath);

            Status = !firstCopyConfirmed
                && result.ErrorCode == SaveErrorCode.FirstCopyConfirmationRequired
                ? $"First-copy confirmation required. Source: {selectedSaveSourcePath} Destination: {dedicatedSavePath}"
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

    private async Task DiscoverProfilesOnceAsync()
    {
        if (_saveProfilesDiscovered)
        {
            return;
        }
        var profiles = await Service.DiscoverSaveProfilesAsync(CancellationToken.None);
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

    private async Task LaunchAsync()
    {
        IsBusy = true;
        try
        {
            var selection = SelectedSaveProfile ?? throw new InvalidOperationException(
                "Select a SteamID before launch.");
            Status = "Launching modded copy. Steam Offline Mode is a user-managed prerequisite.";
            var result = await Service.LaunchModdedAsync(
                selection.SteamId,
                CancellationToken.None);
            Status = result.Started
                ? $"Modded copy exited with code {result.ExitCode ?? 0}. Original and Overhaul remain untouched."
                : $"Modded launch failed: {result.ErrorCode}";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"Modded launch failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task SaveExternalRootAsync()
    {
        IsBusy = true;
        try
        {
            var store = _externalRootStore ?? throw new InvalidOperationException(
                "External-root selection is unavailable.");
            await store.WriteAsync(ExternalRootPath, GamePath, CancellationToken.None);
            var savedRoot = await store.ReadAsync(CancellationToken.None);
            _restartRequired = !string.Equals(
                _materialRoot,
                savedRoot,
                StringComparison.OrdinalIgnoreCase);
            Status = _restartRequired
                ? "External root saved. Restart the launcher before material operations use the new root."
                : "External root is already selected for this launcher session.";
        }
        catch (Exception exception)
        {
            await LogWithoutMaskingAsync(exception);
            Status = $"External-root selection failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool CanMutate() => CanPrepareSave()
        && !string.IsNullOrWhiteSpace(GamePath);

    private bool CanPrepareSave() => _materialOperationsAvailable
        && !IsBusy
        && !_restartRequired
        && _service is not null;

    private void RaiseCommandStates()
    {
        VerifyCommand.RaiseCanExecuteChanged();
        InitializeCommand.RaiseCanExecuteChanged();
        PrepareSaveCommand.RaiseCanExecuteChanged();
        LaunchCommand.RaiseCanExecuteChanged();
        SaveExternalRootCommand.RaiseCanExecuteChanged();
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

    private ILauncherService Service => _service ?? throw new InvalidOperationException(
        "EXTERNAL_ROOT_NOT_SELECTED");

    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
