// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using System;
using System.Reflection;

using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace CalculatorApp.Utils
{
    /// <summary>
    /// Class providing functionality around switching and restoring theme settings
    /// </summary>
    public static class ThemeHelper
    {
        private const string SelectedAppThemeKey = "CalculatorSelectedAppTheme";
        private static WeakReference s_themeRoot;

        private static FrameworkElement ThemeRoot
        {
            get
            {
                if (s_themeRoot?.IsAlive == true && s_themeRoot.Target is FrameworkElement calculatorRoot)
                {
                    return calculatorRoot;
                }

                return Window.Current.Content as FrameworkElement;
            }
        }

        public static void SetThemeRoot(FrameworkElement rootElement)
        {
            s_themeRoot = rootElement == null ? null : new WeakReference(rootElement);
        }

        public static void ClearThemeRoot(FrameworkElement rootElement)
        {
            if (s_themeRoot?.IsAlive == true && ReferenceEquals(s_themeRoot.Target, rootElement))
            {
                s_themeRoot = null;
            }
        }

        /// <summary>
        /// Get or set (with LocalSettings persistence) the RequestedTheme of the root element.
        /// </summary>
        public static ElementTheme RootTheme
        {
            get
            {
                if (ThemeRoot is FrameworkElement rootElement)
                {
                    return rootElement.RequestedTheme;
                }

                return ElementTheme.Default;
            }
            set
            {
                if (ThemeRoot is FrameworkElement rootElement)
                {
                    rootElement.RequestedTheme = value;

                    ApplicationData.Current.LocalSettings.Values[SelectedAppThemeKey] = rootElement.RequestedTheme.ToString();
                }
            }
        }

        public static TEnum GetEnum<TEnum>(string text) where TEnum : struct
        {
            if (!typeof(TEnum).GetTypeInfo().IsEnum)
            {
                throw new InvalidOperationException("Generic parameter 'TEnum' must be an enum.");
            }
            return (TEnum)Enum.Parse(typeof(TEnum), text);
        }

        public static void InitializeAppTheme()
        {
            string savedTheme = ApplicationData.Current.LocalSettings.Values[SelectedAppThemeKey]?.ToString();

            if (!string.IsNullOrEmpty(savedTheme))
            {
                RootTheme = GetEnum<ElementTheme>(savedTheme);
            }
        }

        public static bool IsDarkTheme
        {
            get
            {
                ElementTheme theme = RootTheme;
                return theme == ElementTheme.Dark ||
                    (theme == ElementTheme.Default && Application.Current.RequestedTheme == ApplicationTheme.Dark);
            }
        }

        public struct ThemeChangedCallbackToken
        {
            public WeakReference RootElement;
            public long Token;
        }

        public static ThemeChangedCallbackToken RegisterAppThemeChangedCallback(DependencyPropertyChangedCallback callback)
        {
            FrameworkElement rootElement = ThemeRoot;
            long token = rootElement.RegisterPropertyChangedCallback(FrameworkElement.RequestedThemeProperty, callback);
            return new ThemeChangedCallbackToken { RootElement = new WeakReference(rootElement), Token = token };
        }

        public static void UnregisterAppThemeChangedCallback(ThemeChangedCallbackToken callbackToken)
        {
            if (callbackToken.RootElement?.IsAlive == true)
            {
                FrameworkElement rootElement = callbackToken.RootElement.Target as FrameworkElement;
                rootElement?.UnregisterPropertyChangedCallback(FrameworkElement.RequestedThemeProperty, callbackToken.Token);
            }
        }
    }
}
