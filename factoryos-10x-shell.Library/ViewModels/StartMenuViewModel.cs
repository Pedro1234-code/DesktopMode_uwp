using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using factoryos_10x_shell.Library.Models.InternalData;
using factoryos_10x_shell.Library.Services.Helpers;
using factoryos_10x_shell.Library.Services.Managers;
using factoryos_10x_shell.Library.Services.WebApps;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.System;
using Windows.UI.Input.Preview.Injection;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media.Imaging;

namespace factoryos_10x_shell.Library.ViewModels
{
    public partial class StartMenuViewModel : ObservableObject
    {
        private readonly IStartManagerService m_startManager;

        private readonly IAppHelper m_appHelper;
        private readonly IWebAppService m_webAppService;


        public StartMenuViewModel(IStartManagerService startManager, IAppHelper appHelper, IWebAppService webAppService) 
        {
            m_startManager = startManager;
            m_appHelper = appHelper;
            m_webAppService = webAppService;

            StartIconModel filesIcon = m_appHelper.StartIcons.FirstOrDefault(app => app.AppId == "CoreShell.Files");
            if (filesIcon == null)
            {
                filesIcon = new StartIconModel
                {
                    AppId = "CoreShell.Files",
                    IconName = "Files"
                };
                m_appHelper.StartIcons.Insert(0, filesIcon);
            }
            filesIcon.IconGlyph = null;
            filesIcon.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/Files/files.png"));

            StartIconModel notepadIcon = m_appHelper.StartIcons.FirstOrDefault(app => app.AppId == "CoreShell.Notepad");
            if (notepadIcon == null)
            {
                notepadIcon = new StartIconModel { AppId = "CoreShell.Notepad", IconName = "Notepad", IconGlyph = "\uE8A5" };
                m_appHelper.StartIcons.Insert(1, notepadIcon);
            }
            notepadIcon.IconGlyph = null;
            notepadIcon.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/Notepad/notepad.png"));

            StartIconModel settingsIcon = m_appHelper.StartIcons.FirstOrDefault(app => app.AppId == "CoreShell.Settings");
            if (settingsIcon == null)
            {
                settingsIcon = new StartIconModel { AppId = "CoreShell.Settings", IconName = "Settings" };
                m_appHelper.StartIcons.Insert(2, settingsIcon);
            }
            settingsIcon.IconGlyph = null;
            settingsIcon.IconSource = new BitmapImage(new Uri("ms-appx:///Windows10x-js-main/Icons/WindowsSettings.png"));

            StartIconModel calculatorIcon = m_appHelper.StartIcons.FirstOrDefault(app => app.AppId == "CoreShell.Calculator");
            if (calculatorIcon == null)
            {
                calculatorIcon = new StartIconModel { AppId = "CoreShell.Calculator", IconName = "Calculator" };
                m_appHelper.StartIcons.Insert(3, calculatorIcon);
            }
            calculatorIcon.IconGlyph = null;
            calculatorIcon.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/Calculator/CalculatorAppList.targetsize-48.png"));

            AppsListGridHeight = 310;
            AppsListToggleContent = "Show all";
        }


        public ObservableCollection<StartIconModel> StartIcons => m_appHelper.StartIcons;
        public ObservableCollection<WebAppDefinition> WebApps => m_webAppService.InstalledApps;


        [ObservableProperty]
        private double appsListGridHeight;

        [ObservableProperty]
        private string appsListToggleContent;

        [RelayCommand]
        public void AppsListToggleClicked()
        {
            m_startManager.IsAppsListExpanded = !m_startManager.IsAppsListExpanded;
            bool expanded = m_startManager.IsAppsListExpanded;

            AppsListToggleContent = expanded ? "Show less" : "Show all";
            if (expanded)
            {
                AppsListGridHeight = Double.NaN;
            }
            else
            {
                AppsListGridHeight = 310;
            }
        }



        [RelayCommand]
        public void TextBoxSearchClicked()
        {
            InputInjector inputInjector = InputInjector.TryCreate();

            // Create an instance for the 'Tab' key
            InjectedInputKeyboardInfo sKey = new InjectedInputKeyboardInfo();
            sKey.VirtualKey = (ushort)(VirtualKey.S);
            sKey.KeyOptions = InjectedInputKeyOptions.None;

            // Create an instance for the 'Windows' key
            InjectedInputKeyboardInfo winKey = new InjectedInputKeyboardInfo();
            winKey.VirtualKey = (ushort)(VirtualKey.LeftWindows);
            winKey.KeyOptions = InjectedInputKeyOptions.None;

            // Inject the 'Windows' key down
            inputInjector.InjectKeyboardInput(new[] { winKey });

            // Inject the 'Tab' key down and up
            inputInjector.InjectKeyboardInput(new[] { sKey });
            sKey.KeyOptions = InjectedInputKeyOptions.KeyUp;
            inputInjector.InjectKeyboardInput(new[] { sKey });

            // Inject the 'Windows' key up
            winKey.KeyOptions = InjectedInputKeyOptions.KeyUp;
            inputInjector.InjectKeyboardInput(new[] { winKey });

        }

    }
}
