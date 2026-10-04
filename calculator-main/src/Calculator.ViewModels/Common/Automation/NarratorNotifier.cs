// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using Windows.UI.Xaml;
using Windows.UI.Xaml.Automation.Peers;
using Windows.UI.Xaml.Controls;

namespace CalculatorApp.ViewModel.Common.Automation
{
    public sealed class NarratorNotifier : DependencyObject
    {
        private UIElement _announcementElement;

        // Register the property as part of the type itself. The original standalone
        // Calculator registered it from CalculatorApp.App, but that App constructor is
        // never executed when the UI is hosted by DesktopMode. That left the field null
        // and every x:Bind update failed in DependencyObject.SetValue with E_INVALIDARG.
        public static readonly DependencyProperty AnnouncementProperty =
            DependencyProperty.Register(
                nameof(Announcement),
                typeof(NarratorAnnouncement),
                typeof(NarratorNotifier),
                new PropertyMetadata(null, OnAnnouncementChanged));

        public NarratorNotifier()
        {
        }

        public NarratorAnnouncement Announcement
        {
            get => GetAnnouncement(this);
            set => SetAnnouncement(this, value);
        }

        public static NarratorAnnouncement GetAnnouncement(DependencyObject element)
        {
            return (NarratorAnnouncement)element.GetValue(AnnouncementProperty);
        }

        public static void SetAnnouncement(DependencyObject element, NarratorAnnouncement value)
        {
            element.SetValue(AnnouncementProperty, value);
        }

        public static void RegisterDependencyProperties()
        {
            // Kept for source compatibility with the standalone Calculator App.
            // Referencing this type has already initialized AnnouncementProperty.
            _ = AnnouncementProperty;
        }

        public void Announce(NarratorAnnouncement announcement)
        {
            if (NarratorAnnouncement.IsValid(announcement))
            {
                if (_announcementElement == null)
                {
                    _announcementElement = new TextBlock();
                }

                var peer = FrameworkElementAutomationPeer.FromElement(_announcementElement);
                if (peer != null)
                {
                    peer.RaiseNotificationEvent(
                        announcement.Kind,
                        announcement.Processing,
                        announcement.Announcement,
                        announcement.ActivityId);
                }
            }
        }

        private static void OnAnnouncementChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs e)
        {
            if (dependencyObject is NarratorNotifier instance)
            {
                instance.Announce(e.NewValue as NarratorAnnouncement);
            }
        }
    }
}
