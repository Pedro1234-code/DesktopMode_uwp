using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Text.Json;
using System.Threading.Tasks;

using Windows.ApplicationModel.UserActivities;
using Windows.Foundation;
using Windows.Graphics.Display;
using Windows.Storage;
using Windows.UI.Core;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Automation;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Navigation;
using Microsoft.UI.Xaml.Controls;

using CalculatorApp.Common;
using CalculatorApp.Converters;
using CalculatorApp.JsonUtils;
using CalculatorApp.ViewModel;
using CalculatorApp.ViewModel.Common;
using CalculatorApp.ViewModel.Common.Automation;

using wuxc = Windows.UI.Xaml.Controls;

namespace CalculatorApp
{
    public sealed partial class MainPage : wuxc.Page
    {
        public static string DiagnosticStage { get; private set; } = "Calculator has not started loading";

        public static void SetDiagnosticStage(string stage)
        {
            DiagnosticStage = stage ?? "Unknown Calculator stage";
            System.Diagnostics.Debug.WriteLine($"Calculator stage: {DiagnosticStage}");
        }

        public static readonly DependencyProperty NavViewCategoriesSourceProperty =
            DependencyProperty.Register(nameof(NavViewCategoriesSource), typeof(List<object>), typeof(MainPage), new PropertyMetadata(default));

        public List<object> NavViewCategoriesSource
        {
            get => (List<object>)GetValue(NavViewCategoriesSourceProperty);
            set => SetValue(NavViewCategoriesSourceProperty, value);
        }

        public ApplicationViewModel ViewModel { get; }

        public MainPage()
        {
            SetDiagnosticStage("creating ApplicationViewModel");
            ViewModel = new ApplicationViewModel();
            SetDiagnosticStage("creating navigation categories");
            InitializeNavViewCategoriesSource();
            SetDiagnosticStage("loading MainPage XAML");
            InitializeComponent();
            ThemeHelper.SetThemeRoot(this);
            ThemeHelper.InitializeAppTheme();

            SetDiagnosticStage("initializing keyboard shortcuts");
            KeyboardShortcutManager.Initialize();

            SetDiagnosticStage("subscribing Calculator lifecycle events");
            Application.Current.Suspending += App_Suspending;
            ViewModel.PropertyChanged += OnAppPropertyChanged;
            SetDiagnosticStage("creating accessibility settings");
            m_accessibilitySettings = new AccessibilitySettings();

            SetDiagnosticStage("reading integrated display information");
            if (Utilities.GetIntegratedDisplaySize(out var sizeInInches))
            {
                if (sizeInInches < 7.0) // If device's display size (diagonal length) is less than 7 inches then keep the calc always in Portrait mode only
                {
                    DisplayInformation.AutoRotationPreferences = DisplayOrientations.Portrait | DisplayOrientations.PortraitFlipped;
                }
            }

        }

        public void UnregisterEventHandlers()
        {
            Application.Current.Suspending -= App_Suspending;
            ViewModel.PropertyChanged -= OnAppPropertyChanged;
            SizeChanged -= MainPage_SizeChanged;
            m_accessibilitySettings.HighContrastChanged -= OnHighContrastChanged;
            KeyboardShortcutManager.Uninitialize();
            ThemeHelper.ClearThemeRoot(this);

            if (m_calculator != null)
            {
                m_calculator.UnregisterEventHandlers();
            }
        }

        public void SetDefaultFocus()
        {
            if (m_calculator != null && m_calculator.Visibility == Visibility.Visible)
            {
                m_calculator.SetDefaultFocus();
            }
            if (m_dateCalculator != null && m_dateCalculator.Visibility == Visibility.Visible)
            {
                m_dateCalculator.SetDefaultFocus();
            }
            if (m_graphingCalculator != null && m_graphingCalculator.Visibility == Visibility.Visible)
            {
                m_graphingCalculator.SetDefaultFocus();
            }
            if (m_converter != null && m_converter.Visibility == Visibility.Visible)
            {
                m_converter.SetDefaultFocus();
            }
        }

        public void SetHeaderAutomationName()
        {
            ViewMode mode = ViewModel.Mode;
            var resProvider = AppResourceProvider.GetInstance();

            string name;
            if (NavCategory.IsDateCalculatorViewMode(mode))
            {
                name = resProvider.GetResourceString("HeaderAutomationName_Date");
            }
            else
            {
                string full = string.Empty;
                if (NavCategory.IsCalculatorViewMode(mode) || NavCategory.IsGraphingCalculatorViewMode(mode))
                {
                    full = resProvider.GetResourceString("HeaderAutomationName_Calculator");
                }
                else if (NavCategory.IsConverterViewMode(mode))
                {
                    full = resProvider.GetResourceString("HeaderAutomationName_Converter");
                }
                name = LocalizationStringUtil.GetLocalizedString(full, ViewModel.CategoryName);
            }

            AutomationProperties.SetName(Header, name);
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            SetDiagnosticStage("reading the saved Calculator mode");
            var initialMode = ViewMode.Standard;
            var localSettings = ApplicationData.Current.LocalSettings;
            if (localSettings.Values.ContainsKey(nameof(ApplicationViewModel.Mode)))
            {
                initialMode = NavCategoryStates.Deserialize(localSettings.Values[nameof(ApplicationViewModel.Mode)]);
            }

            if (e.Parameter == null)
            {
                SetDiagnosticStage("initializing the Calculator view model");
                ViewModel.Initialize(initialMode);
                return;
            }

            if (e.Parameter is string legacyArgs)
            {
                if (legacyArgs.Length > 0)
                {
                    initialMode = (ViewMode)Convert.ToInt32(legacyArgs);
                }
                ViewModel.Initialize(initialMode);
            }
            else if (e.Parameter is SnapshotLaunchArguments snapshotArgs)
            {
                ViewModel.Initialize(initialMode);
                bool restored = false;
                if (!snapshotArgs.HasError)
                {
                    try
                    {
                        ViewModel.RestoreFromSnapshot(snapshotArgs.Snapshot);
                        restored = true;
                        TraceLogger.GetInstance().LogRecallRestore((ViewMode)snapshotArgs.Snapshot.Mode);
                    }
                    catch (Exception ex)
                    {
                        TraceLogger.GetInstance().LogRecallError($"OnNavigatedTo:Restore failed. {ex.Message}");
                    }
                }

                if (!restored)
                {
                    _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal,
                        async () => await ShowSnapshotLaunchErrorAsync());
                    if (snapshotArgs.HasError)
                    {
                        TraceLogger.GetInstance().LogRecallError("OnNavigatedTo:Found errors.");
                    }
                }
            }
            else
            {
                Environment.FailFast("cd75d5af-0f47-4cc2-910c-ed792ed16fe6");
            }
        }

        private void InitializeNavViewCategoriesSource()
        {
            NavViewCategoriesSource = ExpandNavViewCategoryGroups(ViewModel.Categories);
            _ = Dispatcher.RunAsync(CoreDispatcherPriority.Low, () =>
            {
                var graphCategory = (NavCategory)NavViewCategoriesSource.Find(x =>
                {
                    if (x is NavCategory category)
                    {
                        return category.ViewMode == ViewMode.Graphing;
                    }
                    else
                    {
                        return false;
                    }
                });
                graphCategory.IsEnabled = NavCategoryStates.IsViewModeEnabled(ViewMode.Graphing);
            });
        }

        private List<object> ExpandNavViewCategoryGroups(IEnumerable<NavCategoryGroup> groups)
        {
            var result = new List<object>();
            foreach (var group in groups)
            {
                result.Add(group);
                foreach (var category in group.Categories)
                {
                    result.Add(category);
                }
            }
            return result;
        }

        private void UpdatePopupSize(Size size)
        {
            if (PopupContent != null)
            {
                PopupContent.Width = size.Width;
                PopupContent.Height = size.Height;
            }
        }

        private void MainPage_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            // We don't use layout aware page's view states, we have our own
            UpdateViewState();
            UpdatePopupSize(e.NewSize);
        }

        private void OnAppPropertyChanged(object sender, PropertyChangedEventArgs e)
        {
            string propertyName = e.PropertyName;
            if (propertyName == nameof(ApplicationViewModel.Mode))
            {
                ViewMode newValue = ViewModel.Mode;
                ViewMode previousMode = ViewModel.PreviousMode;

                KeyboardShortcutManager.DisableShortcuts(false);

                switch (newValue)
                {
                    case ViewMode.Standard:
                        EnsureCalculator();
                        ViewModel.CalculatorViewModel.HistoryVM.AreHistoryShortcutsEnabled = true;
                        m_calculator.AnimateCalculator(NavCategory.IsConverterViewMode(previousMode));
                        ViewModel.CalculatorViewModel.HistoryVM.ReloadHistory(newValue);
                        break;
                    case ViewMode.Scientific:
                        EnsureCalculator();
                        ViewModel.CalculatorViewModel.HistoryVM.AreHistoryShortcutsEnabled = true;
                        if (ViewModel.PreviousMode != ViewMode.Scientific)
                        {
                            m_calculator.AnimateCalculator(NavCategory.IsConverterViewMode(previousMode));
                        }
                        ViewModel.CalculatorViewModel.HistoryVM.ReloadHistory(newValue);
                        break;
                    case ViewMode.Programmer:
                        ViewModel.CalculatorViewModel.HistoryVM.AreHistoryShortcutsEnabled = false;
                        EnsureCalculator();
                        if (ViewModel.PreviousMode != ViewMode.Programmer)
                        {
                            m_calculator.AnimateCalculator(NavCategory.IsConverterViewMode(previousMode));
                        }
                        break;
                    case ViewMode.Graphing:
                        EnsureGraphingCalculator();
                        KeyboardShortcutManager.DisableShortcuts(true);
                        break;
                    default:
                        if (NavCategory.IsDateCalculatorViewMode(newValue))
                        {
                            if (ViewModel.CalculatorViewModel != null)
                            {
                                ViewModel.CalculatorViewModel.HistoryVM.AreHistoryShortcutsEnabled = false;
                            }
                            EnsureDateCalculator();
                        }
                        else if (NavCategory.IsConverterViewMode(newValue))
                        {
                            if (ViewModel.CalculatorViewModel != null)
                            {
                                ViewModel.CalculatorViewModel.HistoryVM.AreHistoryShortcutsEnabled = false;
                            }

                            EnsureConverter();
                            if (!NavCategory.IsConverterViewMode(previousMode))
                            {
                                m_converter.AnimateConverter();
                            }
                        }
                        break;
                }

                ShowHideControls(newValue);

                UpdateViewState();
                SetDefaultFocus();
            }
            else if (propertyName == nameof(ApplicationViewModel.CategoryName))
            {
                SetHeaderAutomationName();
                AnnounceCategoryName();
            }
        }

        private void SelectNavigationItemByModel()
        {
            var menuItems = (List<object>)NavView.MenuItemsSource;
            var itemCount = menuItems.Count;
            var flatIndex = NavCategoryStates.GetFlatIndex(ViewModel.Mode);

            if (flatIndex >= 0 && flatIndex < itemCount)
            {
                NavView.SelectedItem = menuItems[flatIndex];
            }
        }

        private void OnNavLoaded(object sender, RoutedEventArgs e)
        {
            SetDiagnosticStage("loading the Calculator navigation view");
            if (NavView.SelectedItem == null)
            {
                SelectNavigationItemByModel();
            }

            var acceleratorList = new List<MyVirtualKey>();
            NavCategoryStates.GetCategoryAcceleratorKeys(acceleratorList);

            foreach (var accelerator in acceleratorList)
            {
                NavView.SetValue(KeyboardShortcutManager.VirtualKeyAltChordProperty, accelerator);
            }
            // Special case logic for Ctrl+E accelerator for Date Calculation Mode
            NavView.SetValue(KeyboardShortcutManager.VirtualKeyControlChordProperty, MyVirtualKey.E);
        }

        private void OnNavPaneOpened(NavigationView sender, object args)
        {
            KeyboardShortcutManager.HonorShortcuts(false);
            TraceLogger.GetInstance().LogNavBarOpened();
        }

        private void OnNavPaneClosed(NavigationView sender, object args)
        {
            if (Popup.IsOpen)
            {
                return;
            }

            if (ViewModel.Mode != ViewMode.Graphing)
            {
                KeyboardShortcutManager.HonorShortcuts(true);
            }

            SetDefaultFocus();
        }

        private void EnsurePopupContent()
        {
            if (PopupContent == null)
            {
                FindName("PopupContent");

                PopupContent.Width = ActualWidth;
                PopupContent.Height = ActualHeight;
            }
        }

        private void ShowSettingsPopup()
        {
            EnsurePopupContent();
            Popup.IsOpen = true;
        }

        private void CloseSettingsPopup()
        {
            Popup.IsOpen = false;
            SelectNavigationItemByModel();
            SetDefaultFocus();
        }

        private void Popup_Opened(object sender, object e)
        {
            KeyboardShortcutManager.IgnoreEscape(false);
            KeyboardShortcutManager.HonorShortcuts(false);
        }

        private void Popup_Closed(object sender, object e)
        {
            KeyboardShortcutManager.HonorEscape();
            KeyboardShortcutManager.HonorShortcuts(!NavView.IsPaneOpen);
        }

        private void OnNavSelectionChanged(object sender, NavigationViewSelectionChangedEventArgs e)
        {
            if (e.IsSettingsSelected)
            {
                ShowSettingsPopup();
                return;
            }

            if (e.SelectedItemContainer is NavigationViewItem item)
            {
                ViewModel.Mode = (ViewMode)item.Tag;
            }
        }

        private void OnNavItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs e)
        {
            NavView.IsPaneOpen = false;
        }

        private void AlwaysOnTopButtonClick(object sender, RoutedEventArgs e)
        {
            ViewModel.ToggleAlwaysOnTop(0, 0);
        }

        private void TitleBarAlwaysOnTopButtonClick(object sender, RoutedEventArgs e)
        {
            // Always-on-top is owned by the DesktopMode window manager.
        }

        private void ShowHideControls(ViewMode mode)
        {
            var isCalcViewMode = NavCategory.IsCalculatorViewMode(mode);
            var isDateCalcViewMode = NavCategory.IsDateCalculatorViewMode(mode);
            var isGraphingCalcViewMode = NavCategory.IsGraphingCalculatorViewMode(mode);
            var isConverterViewMode = NavCategory.IsConverterViewMode(mode);

            if (m_calculator != null)
            {
                m_calculator.Visibility = BooleanToVisibilityConverter.Convert(isCalcViewMode);
                m_calculator.IsEnabled = isCalcViewMode;
            }

            if (m_dateCalculator != null)
            {
                m_dateCalculator.Visibility = BooleanToVisibilityConverter.Convert(isDateCalcViewMode);
                m_dateCalculator.IsEnabled = isDateCalcViewMode;
            }

            if (m_graphingCalculator != null)
            {
                m_graphingCalculator.Visibility = BooleanToVisibilityConverter.Convert(isGraphingCalcViewMode);
                m_graphingCalculator.IsEnabled = isGraphingCalcViewMode;
            }

            if (m_converter != null)
            {
                m_converter.Visibility = BooleanToVisibilityConverter.Convert(isConverterViewMode);
                m_converter.IsEnabled = isConverterViewMode;
            }
        }

        private void UpdateViewState()
        {
            // All layout related view states are now handled only inside individual controls (standard, scientific, programmer, date, converter)
            if (NavCategory.IsConverterViewMode(ViewModel.Mode))
            {
                int modeIndex = NavCategoryStates.GetIndexInGroup(ViewModel.Mode, CategoryGroupType.Converter);
                if (ViewModel.ConverterViewModel != null && modeIndex >= 0 && modeIndex < (ViewModel.ConverterViewModel.Categories?.Count ?? 0))
                {
                    ViewModel.ConverterViewModel.CurrentCategory = ViewModel.ConverterViewModel.Categories[modeIndex];
                }
            }
        }

        private void UpdatePanelViewState()
        {
            if (m_calculator != null)
            {
                m_calculator.UpdatePanelViewState();
            }
        }

        private void OnHighContrastChanged(AccessibilitySettings sender, object args)
        {
            UpdateViewState();
        }

        private void OnPageLoaded(object sender, RoutedEventArgs args)
        {
            SetDiagnosticStage("loading the Calculator page");
            if (m_converter == null && m_calculator == null && m_dateCalculator == null && m_graphingCalculator == null)
            {
                // We have just launched into our default mode (standard calc) so ensure calc is loaded
                SetDiagnosticStage("ensuring the standard Calculator control");
                EnsureCalculator();
                SetDiagnosticStage("selecting standard Calculator mode");
                ViewModel.CalculatorViewModel.IsStandard = true;
            }

            SetDiagnosticStage("subscribing page layout and accessibility events");
            SizeChanged += MainPage_SizeChanged;
            m_accessibilitySettings.HighContrastChanged += OnHighContrastChanged;
            SetDiagnosticStage("applying the Calculator page visual state");
            UpdateViewState();

            SetDiagnosticStage("setting Calculator accessibility labels");
            SetHeaderAutomationName();
            SetDiagnosticStage("setting initial Calculator focus");
            SetDefaultFocus();
            SetDiagnosticStage("Calculator page loaded successfully");
        }

        private void App_Suspending(object sender, Windows.ApplicationModel.SuspendingEventArgs e)
        {
            if (ViewModel.IsAlwaysOnTop)
            {
                ApplicationDataContainer localSettings = ApplicationData.Current.LocalSettings;
                localSettings.Values[ApplicationViewModel.WidthLocalSettingsKey] = ActualWidth;
                localSettings.Values[ApplicationViewModel.HeightLocalSettingsKey] = ActualHeight;
            }
        }

        private void EnsureCalculator()
        {
            if (m_calculator == null)
            {
                var stage = "creating the Calculator control";
                try
                {
                    var calcVM = ViewModel.CalculatorViewModel;

                    // In C#, CalculatorViewModel is lazily created in OnModeChanged.
                    // If EnsureCalculator is called before mode is set, force creation.
                    if (calcVM == null)
                    {
                        stage = "initializing the standard view model";
                        ViewModel.Mode = ViewMode.Standard;
                        calcVM = ViewModel.CalculatorViewModel;
                    }

                    stage = "creating the Calculator XAML tree";
                    m_calculator = new Calculator();
                    stage = "attaching the Calculator view model";
                    m_calculator.ViewModel = calcVM;
                    m_calculator.Name = "Calculator";
                    m_calculator.DataContext = calcVM;
                    Binding isStandardBinding = new Binding
                    {
                        Path = new PropertyPath("IsStandard")
                    };
                    m_calculator.SetBinding(Calculator.IsStandardProperty, isStandardBinding);
                    Binding isScientificBinding = new Binding
                    {
                        Path = new PropertyPath("IsScientific")
                    };
                    m_calculator.SetBinding(Calculator.IsScientificProperty, isScientificBinding);
                    Binding isProgramerBinding = new Binding
                    {
                        Path = new PropertyPath("IsProgrammer")
                    };
                    m_calculator.SetBinding(Calculator.IsProgrammerProperty, isProgramerBinding);
                    Binding isAlwaysOnTopBinding = new Binding
                    {
                        Path = new PropertyPath("IsAlwaysOnTop")
                    };
                    m_calculator.SetBinding(Calculator.IsAlwaysOnTopProperty, isAlwaysOnTopBinding);
                    m_calculator.Style = CalculatorBaseStyle;

                    stage = "placing the Calculator in its DesktopMode host";
                    CalcHolder.Child = m_calculator;

                    // Calculator's "default" state is visible, but if we get delay loaded
                    // when in converter, we should not be visible. This is not a problem for converter
                    // since its default state is hidden.
                    stage = "applying the initial Calculator mode";
                    ShowHideControls(ViewModel.Mode);
                }
                catch (Exception ex)
                {
                    m_calculator = null;
                    throw new InvalidOperationException($"Calculator initialization failed while {stage}.", ex);
                }
            }

            if (m_dateCalculator != null)
            {
                m_dateCalculator.CloseCalendarFlyout();
            }
        }

        private void EnsureDateCalculator()
        {
            if (m_dateCalculator == null)
            {
                m_dateCalculator = new DateCalculator();
                m_dateCalculator.ViewModel = ViewModel.DateCalcViewModel;
                m_dateCalculator.DataContext = ViewModel.DateCalcViewModel;
                m_dateCalculator.Name = "dateCalculator";
                DateCalcHolder.Child = m_dateCalculator;
            }

            if (m_calculator != null)
            {
                m_calculator.CloseHistoryFlyout();
                m_calculator.CloseMemoryFlyout();
            }
        }

        private void EnsureGraphingCalculator()
        {
            if (m_graphingCalculator == null)
            {
                m_graphingCalculator = new GraphingCalculator
                {
                    Name = "GraphingCalculator",
                    DataContext = ViewModel.GraphingCalcViewModel
                };

                GraphingCalcHolder.Child = m_graphingCalculator;
            }
        }

        private void EnsureConverter()
        {
            if (m_converter == null)
            {
                // delay loading converter
                m_converter = new CalculatorApp.UnitConverter
                {
                    Name = "unitConverter",
                    Style = UnitConverterBaseStyle
                };
                m_converter.ViewModel = ViewModel.ConverterViewModel;
                m_converter.DataContext = ViewModel.ConverterViewModel;
                ConverterHolder.Child = m_converter;
            }
        }

        private void AnnounceCategoryName()
        {
            string categoryName = AutomationProperties.GetName(Header);
            NarratorAnnouncement announcement = CalculatorAnnouncement.GetCategoryNameChangedAnnouncement(categoryName);
            NarratorNotifier.Announce(announcement);
        }

        private bool ShouldShowBackButton(bool isAlwaysOnTop, bool isPopupOpen)
        {
            return !isAlwaysOnTop && isPopupOpen;
        }

        private double NavigationViewOpenPaneLength(bool isAlwaysOnTop)
        {
            return isAlwaysOnTop ? 0 : (double)Application.Current.Resources["SplitViewOpenPaneLength"];
        }

        private GridLength DoubleToGridLength(double value)
        {
            return new GridLength(value);
        }

        private void Settings_BackButtonClick(object sender, RoutedEventArgs e)
        {
            CloseSettingsPopup();
        }

        private async Task ShowSnapshotLaunchErrorAsync()
        {
            var resProvider = AppResourceProvider.GetInstance();
            var dialog = new wuxc.ContentDialog
            {
                Title = resProvider.GetResourceString("AppName"),
                Content = new wuxc.TextBlock { Text = resProvider.GetResourceString("SnapshotRestoreError") },
                CloseButtonText = resProvider.GetResourceString("ErrorButtonOk"),
                DefaultButton = wuxc.ContentDialogButton.Close
            };
            await dialog.ShowAsync();
        }

        private Calculator m_calculator;
        private GraphingCalculator m_graphingCalculator;
        private UnitConverter m_converter;
        private DateCalculator m_dateCalculator;
        private readonly AccessibilitySettings m_accessibilitySettings;
    }
}
