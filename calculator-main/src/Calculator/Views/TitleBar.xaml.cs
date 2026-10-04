using CalculatorApp.ViewModel.Common;

using Windows.ApplicationModel.Core;
using Windows.System.Profile;
using Windows.UI.Core;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

// The User Control item template is documented at https://go.microsoft.com/fwlink/?LinkId=234236

namespace CalculatorApp
{
    public sealed partial class TitleBar : UserControl
    {
        public TitleBar()
        {
            m_uiSettings = new UISettings();
            m_accessibilitySettings = new AccessibilitySettings();
            InitializeComponent();
            Height = 0;
            Visibility = Visibility.Collapsed;
        }

        public bool IsAlwaysOnTopMode
        {
            get => (bool)GetValue(IsAlwaysOnTopModeProperty);
            set => SetValue(IsAlwaysOnTopModeProperty, value);
        }

        // Using a DependencyProperty as the backing store for IsAlwaysOnTopMode.  This enables animation, styling, binding, etc...
        public static readonly DependencyProperty IsAlwaysOnTopModeProperty =
            DependencyProperty.Register(nameof(IsAlwaysOnTopMode), typeof(bool), typeof(TitleBar), new PropertyMetadata(default(bool), (sender, args) =>
            {
                var self = (TitleBar)sender;
                self.OnIsAlwaysOnTopModePropertyChanged((bool)args.OldValue, (bool)args.NewValue);
            }));

        public event Windows.UI.Xaml.RoutedEventHandler AlwaysOnTopClick;

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            Visibility = Visibility.Collapsed;
        }

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
        }

        private void RootFrame_RequestedThemeChanged(DependencyObject sender, DependencyProperty dp)
        {
            if (Frame.RequestedThemeProperty == dp)
            {
                _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, SetTitleBarControlColors);
            }
        }

        private void CoreTitleBarIsVisibleChanged(CoreApplicationViewTitleBar cTitleBar, object args)
        {
            SetTitleBarVisibility(false);
        }

        private void CoreTitleBarLayoutMetricsChanged(CoreApplicationViewTitleBar cTitleBar, object args)
        {
            SetTitleBarHeightAndPadding();
        }

        private void SetTitleBarVisibility(bool forceDisplay)
        {
            LayoutRoot.Visibility = Visibility.Collapsed;
        }

        private void SetTitleBarHeightAndPadding()
        {
            Height = 0;
        }

        private void ColorValuesChanged(Windows.UI.ViewManagement.UISettings sender, object e)
        {
            _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, SetTitleBarControlColors);
        }

        private void SetTitleBarControlColors()
        {
        }

        private void OnHighContrastChanged(Windows.UI.ViewManagement.AccessibilitySettings sender, object args)
        {
            _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                SetTitleBarControlColors();
                SetTitleBarVisibility(false);
            });
        }

        private void OnWindowActivated(object sender, WindowActivatedEventArgs e)
        {
            VisualStateManager.GoToState(
                this, e.WindowActivationState == CoreWindowActivationState.Deactivated ? WindowNotFocused.Name : WindowFocused.Name, false);
        }

        private void OnIsAlwaysOnTopModePropertyChanged(bool oldValue, bool newValue)
        {
            SetTitleBarVisibility(false);
            VisualStateManager.GoToState(this, newValue ? "AOTMiniState" : "AOTNormalState", false);
        }

        private void AlwaysOnTopButton_Click(object sender, RoutedEventArgs e)
        {
            AlwaysOnTopClick?.Invoke(this, e);
        }

        // Dependency properties for the color of the system title bar buttons
        public Windows.UI.Xaml.Media.SolidColorBrush ButtonBackground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonBackgroundProperty);
            set => SetValue(ButtonBackgroundProperty, value);
        }
        public static readonly DependencyProperty ButtonBackgroundProperty =
            DependencyProperty.Register(nameof(ButtonBackground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonForeground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonForegroundProperty);
            set => SetValue(ButtonForegroundProperty, value);
        }
        public static readonly DependencyProperty ButtonForegroundProperty =
            DependencyProperty.Register(nameof(ButtonForeground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonInactiveBackground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonInactiveBackgroundProperty);
            set => SetValue(ButtonInactiveBackgroundProperty, value);
        }
        public static readonly DependencyProperty ButtonInactiveBackgroundProperty =
            DependencyProperty.Register(nameof(ButtonInactiveBackground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonInactiveForeground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonInactiveForegroundProperty);
            set => SetValue(ButtonInactiveForegroundProperty, value);
        }
        public static readonly DependencyProperty ButtonInactiveForegroundProperty =
            DependencyProperty.Register(nameof(ButtonInactiveForeground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonHoverBackground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonHoverBackgroundProperty);
            set => SetValue(ButtonHoverBackgroundProperty, value);
        }
        public static readonly DependencyProperty ButtonHoverBackgroundProperty =
            DependencyProperty.Register(nameof(ButtonHoverBackground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonHoverForeground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonHoverForegroundProperty);
            set => SetValue(ButtonHoverForegroundProperty, value);
        }
        public static readonly DependencyProperty ButtonHoverForegroundProperty =
            DependencyProperty.Register(nameof(ButtonHoverForeground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonPressedBackground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonPressedBackgroundProperty);
            set => SetValue(ButtonPressedBackgroundProperty, value);
        }
        public static readonly DependencyProperty ButtonPressedBackgroundProperty =
            DependencyProperty.Register(nameof(ButtonPressedBackground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public Windows.UI.Xaml.Media.SolidColorBrush ButtonPressedForeground
        {
            get => (Windows.UI.Xaml.Media.SolidColorBrush)GetValue(ButtonPressedForegroundProperty);
            set => SetValue(ButtonPressedForegroundProperty, value);
        }
        public static readonly DependencyProperty ButtonPressedForegroundProperty =
            DependencyProperty.Register(nameof(ButtonPressedForeground), typeof(Windows.UI.Xaml.Media.SolidColorBrush), typeof(TitleBar), new PropertyMetadata(null));

        public bool BackButtonSpaceReserved
        {
            get => (bool)GetValue(BackButtonSpaceReservedProperty);
            set => SetValue(BackButtonSpaceReservedProperty, value);
        }
        public static readonly DependencyProperty BackButtonSpaceReservedProperty =
            DependencyProperty.Register(
                nameof(BackButtonSpaceReserved), typeof(bool), typeof(TitleBar),
                new PropertyMetadata(false, (sender, args) =>
                {
                    var self = sender as TitleBar;
                    VisualStateManager.GoToState(
                        self, (bool)args.NewValue ? self.BackButtonVisible.Name : self.BackButtonCollapsed.Name, true);
                }));

        private readonly Windows.UI.ViewManagement.UISettings m_uiSettings;
        private readonly Windows.UI.ViewManagement.AccessibilitySettings m_accessibilitySettings;
        private Utils.ThemeHelper.ThemeChangedCallbackToken m_rootFrameRequestedThemeCallbackToken;
    }
}
