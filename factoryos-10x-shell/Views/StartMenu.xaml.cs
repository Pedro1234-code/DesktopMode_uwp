using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Services.Helpers;
using factoryos_10x_shell.Library.Services.WebApps;
using Microsoft.Extensions.DependencyInjection;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Runtime.Serialization.Json;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Core;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.Graphics.Imaging;
using Windows.Management.Deployment;
using Windows.Networking.Proximity;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.System;
using Windows.System.UserProfile;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.Xaml.Navigation;
using static factoryos_10x_shell.Helpers.VisualHelper;
using static System.Net.Mime.MediaTypeNames;

namespace factoryos_10x_shell.Views
{
    public sealed partial class StartMenu : Page
    {

        private readonly IAppHelper appHelper;
        private readonly IWebAppService webAppService;
        private readonly IWindowManagerService windowManager;
        private readonly IStartManagerService startManager;

        public StartMenu()
        {
            this.InitializeComponent();
            appHelper = App.ServiceProvider.GetRequiredService<IAppHelper>();
            webAppService = App.ServiceProvider.GetRequiredService<IWebAppService>();
            windowManager = App.ServiceProvider.GetRequiredService<IWindowManagerService>();
            startManager = App.ServiceProvider.GetRequiredService<IStartManagerService>();
            DataContext = App.ServiceProvider.GetRequiredService<StartMenuViewModel>();

        }

        public StartMenuViewModel ViewModel => (StartMenuViewModel)this.DataContext;


        private async void AppButton_Click(object sender, RoutedEventArgs e)
        {
            StartIconModel model = ((FrameworkElement)sender).DataContext as StartIconModel;
            if (model != null)
            {
                if (model.AppId == "CoreShell.Files")
                {
                    AppState.Instance.RequestFilesOpen(true);
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Notepad")
                {
                    AppState.Instance.RequestNotepadOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                if (model.AppId == "CoreShell.Settings")
                {
                    AppState.Instance.RequestSettingsOpen();
                    startManager.RequestStartVisibilityChange(false);
                    return;
                }
                await appHelper.LaunchAppAsync(model);

            }
        }

        private void PinToTaskbar_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem item &&
                item.DataContext is StartIconModel app)
            {
                if (!appHelper.TaskbarIcons.Any(i => i.AppId == app.AppId))
                {
                    appHelper.TaskbarIcons.Add(app);
                    appHelper.SaveTaskbarPinnedApps(appHelper.TaskbarIcons.ToList());
                    Debug.WriteLine($"Salvando: AppId={app.AppId} / Aumid={app.Aumid ?? "NULL"} / Nome={app.IconName}");
                }
            }
        }

        private async void PowerButton_click(object sender, RoutedEventArgs e)
        {
            await Launcher.LaunchUriAsync(new Uri("powerdialogcomponent:"));
        }

        private async void InstallWebAppButton_Click(object sender, RoutedEventArgs e)
        {
            var nameBox = new TextBox { PlaceholderText = "App name (optional)" };
            var urlBox = new TextBox { PlaceholderText = "https://example.com" };
            var panel = new StackPanel { Spacing = 10 };
            panel.Children.Add(nameBox);
            panel.Children.Add(urlBox);
            var dialog = new ContentDialog { Title = "Install web app", Content = panel, PrimaryButtonText = "Install", CloseButtonText = "Cancel" };
            ContentDialogResult installResult;
            MainPage.SetPopupCursorVisibility(true);
            try { installResult = await dialog.ShowAsync(); }
            finally { MainPage.SetPopupCursorVisibility(false); }
            if (installResult != ContentDialogResult.Primary) return;

            try
            {
                var app = await webAppService.InstallAsync(nameBox.Text, urlBox.Text);
                windowManager.Open(app);
                startManager.RequestStartVisibilityChange(false);
            }
            catch (ArgumentException exception)
            {
                await new ContentDialog { Title = "Invalid address", Content = exception.Message, CloseButtonText = "OK" }.ShowAsync();
            }
        }

        private void WebAppButton_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
            {
                windowManager.Open(app);
                startManager.RequestStartVisibilityChange(false);
            }
        }

        private void FilesButton_Click(object sender, RoutedEventArgs e)
        {
            AppState.Instance.RequestFilesOpen(true);
            startManager.RequestStartVisibilityChange(false);
        }

        private void PinWebApp_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
                webAppService.SetPinned(app, true);
        }

        private void NewWebAppWindow_Click(object sender, RoutedEventArgs e)
        {
            if (((FrameworkElement)sender).DataContext is WebAppDefinition app)
            {
                windowManager.OpenNewWindow(app);
                startManager.RequestStartVisibilityChange(false);
            }
        }

        private void UninstallWebApp_Click(object sender, RoutedEventArgs e)
        {
            if (!(((FrameworkElement)sender).DataContext is WebAppDefinition app)) return;
            foreach (var window in windowManager.Windows.Where(window => window.App.Id == app.Id).ToList())
                windowManager.Close(window);
            webAppService.Uninstall(app);
        }

        private void SettingsButton_click(object sender, RoutedEventArgs e)
        {
            AppState.Instance.RequestSettingsOpen();
            startManager.RequestStartVisibilityChange(false);
        }

        private void ContextFlyout_Opened(object sender, object e) => MainPage.SetPopupCursorVisibility(true);
        private void ContextFlyout_Closed(object sender, object e) => MainPage.SetPopupCursorVisibility(false);

    }
}
