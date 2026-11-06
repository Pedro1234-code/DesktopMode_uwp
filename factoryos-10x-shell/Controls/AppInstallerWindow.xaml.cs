using System;
using System.Collections.Generic;
using System.Linq;
using factoryos_10x_shell.Services.Files;
using factoryos_10x_shell.Services.Helpers;
using Microsoft.Extensions.DependencyInjection;
using Windows.Foundation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.Storage.Streams;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class AppInstallerWindow : UserControl
    {
        internal static event Action<StorageFile> OpenRequested;
        internal static void RequestOpen(StorageFile file) => OpenRequested?.Invoke(file);
        internal event Action Changed, ActivationRequested, CloseRequested;
        internal string PackageTitle { get; private set; } = "App Installer";
        private readonly StorageFile m_package;
        private IReadOnlyList<StorageFile> m_dependencies = new List<StorageFile>();
        private bool m_running, m_ready, m_closed, m_installed, m_dragging;
        private Point m_start;
        private double m_left, m_top;
        private AppPackagePreview m_preview;
        private bool m_maximized;
        private Rect m_restoreBounds;
        internal bool OpensPackage(StorageFile file) => m_package?.IsEqual(file) == true;
        internal void ActivateFromTaskbar() => ActivationRequested?.Invoke();

        public AppInstallerWindow() { InitializeComponent(); }
        internal AppInstallerWindow(StorageFile package) : this()
        {
            m_package = package;
            Loaded += LoadPackage;
        }

        private async void LoadPackage(object sender, RoutedEventArgs e)
        {
            Loaded -= LoadPackage;
            try
            {
                var preview = await AppPackageService.ReadAsync(m_package);
                if (m_closed) return;
                m_preview = preview;
                PackageTitle = preview.Name;
                Heading.Text = "Install " + preview.Name + "?";
                Publisher.Text = "Publisher: " + preview.Publisher;
                VersionText.Text = "Version: " + preview.Version;
                Description.Text = preview.Description;
                Description.Visibility = string.IsNullOrWhiteSpace(preview.Description) ? Visibility.Collapsed : Visibility.Visible;
                SourceText.Text = "Source: " + m_package.Name;
                Capabilities.Text = string.IsNullOrEmpty(preview.Capabilities) ? "None declared" :
                    string.Join("\n", preview.Capabilities.Split('\n').Select(name => "• " + CapabilityLabel(name)));
                LaunchWhenReady.IsEnabled = !string.IsNullOrEmpty(preview.ApplicationId);
                LaunchWhenReady.IsChecked = LaunchWhenReady.IsEnabled;
                ResourceNotice.Visibility = preview.HasLocalizedResources ? Visibility.Visible : Visibility.Collapsed;
                if (preview.Logo != null)
                {
                    try
                    {
                        using (var stream = new InMemoryRandomAccessStream())
                        {
                            using (var writer = new DataWriter(stream.GetOutputStreamAt(0))) { writer.WriteBytes(preview.Logo); await writer.StoreAsync(); }
                            stream.Seek(0);
                            var bitmap = new BitmapImage { DecodePixelWidth = 144 };
                            await bitmap.SetSourceAsync(stream);
                            PackageLogo.Source = bitmap;
                            LogoFallback.Visibility = Visibility.Collapsed;
                        }
                    }
                    catch { Status.Text = "The package logo could not be displayed."; }
                }
                m_ready = true;
                InstallButton.IsEnabled = true;
                Changed?.Invoke();
            }
            catch (Exception error)
            {
                Heading.Text = "Unable to open this app package";
                Status.Text = error.Message + " (" + error.GetType().Name + ", 0x" + error.HResult.ToString("X8") + ")";
            }
            finally { InstallProgress.Visibility = Visibility.Collapsed; }
        }

        private async void Dependencies_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                var picker = new FileOpenPicker();
                foreach (string extension in new[] { ".appx", ".appxbundle", ".msix", ".msixbundle" }) picker.FileTypeFilter.Add(extension);
                var files = await picker.PickMultipleFilesAsync();
                if (files.Count == 0 || m_closed) return;
                m_dependencies = files.Where(file => !file.IsEqual(m_package)).ToList();
                DependencyText.Text = string.Join(", ", m_dependencies.Select(file => file.Name));
            }
            catch (Exception error) { Status.Text = error.Message; }
        }

        private async void Install_Click(object sender, RoutedEventArgs e)
        {
            if (m_running || !m_ready) return;
            if (m_installed) { CloseRequested?.Invoke(); return; }
            m_running = true;
            ConnectionHelp.Visibility = Visibility.Collapsed;
            PackageDetailsScroll.Visibility = Visibility.Visible;
            CancelButton.Content = "Hide";
            LaunchWhenReady.IsEnabled = false;
            InstallButton.IsEnabled = false;
            SetInstallOptionsEnabled(false);
            InstallProgress.Visibility = Visibility.Visible;
            Heading.Text = "Installing " + PackageTitle;
            try
            {
                string launchNotice = await AppPackageService.InstallAsync(m_package, m_dependencies, Username.Text, Password.Password,
                    TrustLocalCertificate.IsChecked == true, new Progress<string>(message => { if (m_running) Status.Text = message; }),
                    LaunchWhenReady.IsChecked == true, m_preview.IdentityName, m_preview.ApplicationId);
                m_installed = true;
                CancelButton.Visibility = Visibility.Collapsed;
                Heading.Text = PackageTitle + " is installed!";
                Status.Text = string.IsNullOrEmpty(launchNotice) ? "Installation completed successfully." : launchNotice;
                InstallButton.Content = "Close";
                try
                {
                    await App.ServiceProvider.GetRequiredService<AppHelper>().LoadAppsAsync();
                }
                catch (Exception refreshError)
                {
                    System.Diagnostics.Debug.WriteLine("App Installer: Start Menu refresh failed: 0x" + refreshError.HResult.ToString("X8"));
                    Status.Text += " The Start Menu app list could not be refreshed.";
                }
            }
            catch (Exception error)
            {
                if (error is LocalPortalConnectionException)
                {
                    PackageDetailsScroll.Visibility = Visibility.Collapsed;
                    ConnectionHelp.Visibility = Visibility.Visible;
                    Status.Text = "Could not connect to localhost";
                    InstallButton.Content = "Try again";
                }
                else
                {
                Heading.Text = "Couldn't install " + PackageTitle;
                Status.Text = error.Message + " (0x" + error.HResult.ToString("X8") + ")";
                if (error is UnauthorizedAccessException) { InstallOptions.Visibility = Visibility.Visible; ConnectionOptions.IsOn = true; }
                }
            }
            finally
            {
                Password.Password = string.Empty;
                m_running = false;
                CancelButton.Content = "Close";
                LaunchWhenReady.IsEnabled = !m_installed && !string.IsNullOrEmpty(m_preview.ApplicationId);
                InstallButton.IsEnabled = true;
                SetInstallOptionsEnabled(!m_installed);
                InstallProgress.Visibility = Visibility.Collapsed;
                Changed?.Invoke();
            }
        }

        private void ConnectionOptions_Toggled(object sender, RoutedEventArgs e)
        { if (ConnectionPanel != null) ConnectionPanel.Visibility = ConnectionOptions.IsOn ? Visibility.Visible : Visibility.Collapsed; }
        private void SetInstallOptionsEnabled(bool enabled)
        {
            SetControlsEnabled(InstallOptions, enabled);
        }

        private static void SetControlsEnabled(DependencyObject parent, bool enabled)
        {
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
            {
                var child = VisualTreeHelper.GetChild(parent, i);
                if (child is Control control) control.IsEnabled = enabled;
                else SetControlsEnabled(child, enabled);
            }
        }
        private void Root_PointerPressed(object sender, PointerRoutedEventArgs e) => ActivationRequested?.Invoke();
        private void MoreOptions_Click(object sender, RoutedEventArgs e) => InstallOptions.Visibility =
            InstallOptions.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;

        private static string CapabilityLabel(string name)
        {
            switch (name)
            {
                case "internetClient": return "Access your Internet connection";
                case "internetClientServer": return "Access your Internet connection and act as a server";
                case "privateNetworkClientServer": return "Access your home or work networks";
                case "picturesLibrary": return "Use your pictures library";
                case "videosLibrary": return "Use your video library";
                case "musicLibrary": return "Use your music library";
                case "removableStorage": return "Use data stored on an external storage device";
                case "microphone": return "Use your microphone";
                case "webcam": return "Use your webcam";
                case "runFullTrust": return "Uses all system resources";
                case "broadFileSystemAccess": return "Access your file system";
                default: return name;
            }
        }

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            var host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null) return;
            if (!m_maximized)
            {
                m_restoreBounds = new Rect(Canvas.GetLeft(this), Canvas.GetTop(this), Width, Height);
                Canvas.SetLeft(this, 0); Canvas.SetTop(this, 0);
                Width = host.ActualWidth; Height = host.ActualHeight;
            }
            else
            {
                Canvas.SetLeft(this, m_restoreBounds.X); Canvas.SetTop(this, m_restoreBounds.Y);
                Width = m_restoreBounds.Width; Height = m_restoreBounds.Height;
            }
            m_maximized = !m_maximized;
        }
        internal void CloseWindow()
        {
            if (m_running) { Visibility = Visibility.Collapsed; Status.Text = "Installation continues in the background."; Changed?.Invoke(); return; }
            m_closed = true;
            CloseRequested?.Invoke();
        }
        private void Close_Click(object sender, RoutedEventArgs e) => CloseWindow();
        private void Minimize_Click(object sender, RoutedEventArgs e) { Visibility = Visibility.Collapsed; Changed?.Invoke(); }
        private void Title_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            var host = VisualTreeHelper.GetParent(this) as Canvas;
            if (host == null || m_maximized || !e.GetCurrentPoint(host).Properties.IsLeftButtonPressed) return;
            m_start = e.GetCurrentPoint(host).Position; m_left = Canvas.GetLeft(this); m_top = Canvas.GetTop(this);
            m_dragging = (sender as UIElement)?.CapturePointer(e.Pointer) == true;
        }
        private void Title_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            var host = VisualTreeHelper.GetParent(this) as Canvas;
            if (!m_dragging || host == null) return;
            var point = e.GetCurrentPoint(host).Position;
            Canvas.SetLeft(this, Math.Max(0, Math.Min(Math.Max(0, host.ActualWidth - ActualWidth), m_left + point.X - m_start.X)));
            Canvas.SetTop(this, Math.Max(0, Math.Min(Math.Max(0, host.ActualHeight - 36), m_top + point.Y - m_start.Y)));
        }
        private void Title_PointerReleased(object sender, PointerRoutedEventArgs e) { m_dragging = false; (sender as UIElement)?.ReleasePointerCaptures(); }
    }
}
