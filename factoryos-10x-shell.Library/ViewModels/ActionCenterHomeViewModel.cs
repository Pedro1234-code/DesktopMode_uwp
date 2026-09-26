using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using factoryos_10x_shell.Library.Services.Managers;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.UI.Xaml;

namespace factoryos_10x_shell.Library.ViewModels
{
    public partial class ActionCenterHomeViewModel : ObservableObject
    {
        private readonly IActionCenterManagerService m_actionManager;

        public ActionCenterHomeViewModel(IActionCenterManagerService actionManager) 
        {
            m_actionManager = actionManager;

            ToggleSectionHeight = 100;
            ExpanderText = "\uE70E";
            NetworkStatusCharacter = "\uEB55";
            NetworkName = "Not connected";
            BatteryStatusText = string.Empty;
        }

        #region Toggle expander
        [RelayCommand]
        private void ToggleExpanderClicked()
        {
            if (m_actionManager.IsToggleListExpanded = !m_actionManager.IsToggleListExpanded)
            {
                ToggleSectionHeight = 278;
                ExpanderText = "\uE70D";
            }
            else
            {
                ToggleSectionHeight = 100;
                ExpanderText = "\uE70E";
            }
        }
        [ObservableProperty]
        private int toggleSectionHeight;

        [ObservableProperty]
        private string expanderText;
        #endregion




        #region Battery status
        public Visibility BatteryStatusVisibility => Visibility.Collapsed;
        [ObservableProperty]
        private string batteryStatusText;
        #endregion


        #region Network status
        [ObservableProperty]
        private string networkStatusCharacter;

        [ObservableProperty]
        private string networkName;

        [ObservableProperty]
        private bool networkIsConnected;

        #endregion

        public bool IsBluetoothEnabled => false;
    }
}
