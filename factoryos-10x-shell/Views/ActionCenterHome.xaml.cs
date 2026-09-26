using factoryos_10x_shell.Library.ViewModels;
using factoryos_10x_shell.Controls;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Toolkit.Uwp.Connectivity;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using Windows.System;
using Windows.Media.Playback;
using Windows.Networking.Connectivity;
using Windows.UI.Core;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;

namespace factoryos_10x_shell.Views
{
    public sealed partial class ActionCenterHome : Page
    {
        public static bool connected;
        private bool isExpaneded = false;
        private bool m_initializingVolume = true;
        public ActionCenterHome()
        {
            this.InitializeComponent();

            DataContext = App.ServiceProvider.GetRequiredService<ActionCenterHomeViewModel>();
            Loaded += ActionCenterHome_Loaded;
            Unloaded += ActionCenterHome_Unloaded;
            NetworkControl.ArrowClicked += NetworkControl_ArrowClicked;
        }

        public ActionCenterHomeViewModel ViewModel => (ActionCenterHomeViewModel)this.DataContext;

        private void ActionCenterHome_Loaded(object sender, RoutedEventArgs e)
        {
            try { VolumeSlider.Value = ShellMediaPlayer.Volume; }
            catch { VolumeSlider.Value = 1; }
            m_initializingVolume = false;
            NetworkInformation.NetworkStatusChanged += NetworkInformation_NetworkStatusChanged;
            RefreshNetworkStatus();
        }

        private void ActionCenterHome_Unloaded(object sender, RoutedEventArgs e)
        {
            NetworkInformation.NetworkStatusChanged -= NetworkInformation_NetworkStatusChanged;
        }

        private void VolumeSlider_ValueChanged(object sender, Windows.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (m_initializingVolume) return;
            try { ShellMediaPlayer.Volume = e.NewValue; }
            catch { }
        }

        private static MediaPlayer ShellMediaPlayer
        {
            get
            {
                if (App.MediaPlayer == null)
                    App.MediaPlayer = new MediaPlayer();
                return App.MediaPlayer;
            }
        }

        private async void NetworkInformation_NetworkStatusChanged(object sender)
        {
            await Dispatcher.RunAsync(CoreDispatcherPriority.Normal, RefreshNetworkStatus);
        }

        private void RefreshNetworkStatus()
        {
            try
            {
                ConnectionProfile profile = NetworkInformation.GetInternetConnectionProfile();
                bool isConnected = profile != null &&
                    profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel.InternetAccess;
                if (!isConnected)
                {
                    SetNetworkDisconnected();
                    return;
                }

                // IsWlanConnectionProfile é confiável no Xbox, ao contrário do
                // IANA do adaptador, que por vezes vem como Ethernet em Wi-Fi.
                if (profile.IsWlanConnectionProfile || profile.GetSignalBars() > 0)
                    NetworkControl.Icon = "\uE701";
                else if (profile.IsWwanConnectionProfile)
                    NetworkControl.Icon = "\uE701";
                else
                    NetworkControl.Icon = "\uE839";
                NetworkControl.Subtext = "Connected";
                NetworkControl.IsChecked = true;
            }
            catch { SetNetworkDisconnected(); }
        }

        private void SetNetworkDisconnected()
        {
            NetworkControl.Icon = "\uE774";
            NetworkControl.Subtext = "Not connected";
            NetworkControl.IsChecked = false;
        }

        private async void NetworkControl_ArrowClicked(object sender, EventArgs e)
        {
            await Launcher.LaunchUriAsync(new Uri("ms-settings:network"));
        }

        private async void SettingsButton_Click(object sender, RoutedEventArgs e)
        {
            await Launcher.LaunchUriAsync(new Uri("ms-settings:"));
        }
    }
}
