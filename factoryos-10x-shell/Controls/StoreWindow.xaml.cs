using System;
using System.Linq;
using System.Numerics;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.DependencyInjection;
using Newtonsoft.Json.Linq;
using factoryos_10x_shell.Services.Files;
using factoryos_10x_shell.Services.Store;
using Windows.UI.Composition;
using Windows.UI.Xaml.Hosting;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Animation;
using Windows.UI.Xaml.Media.Imaging;
using Windows.Devices.Input;
using Windows.System;
using Windows.Foundation;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Automation;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using factoryos_10x_shell.Services.Helpers;
using muxc = Microsoft.UI.Xaml.Controls;

namespace factoryos_10x_shell.Controls
{
    public sealed partial class StoreWindow : UserControl
    {
        private const double MinimumWidth = 420;
        private const double MinimumHeight = 320;
        private readonly StorePreviewProduct[] m_offlineHighlights =
        {
            new StorePreviewProduct("9WZDNCRFHWD2", "Microsoft Solitaire Collection", "Games", "Solitaire", "Play Klondike, Spider, FreeCell, Pyramid, and TriPeaks."),
            new StorePreviewProduct("9MSPC6MP8FM4", "Microsoft Whiteboard", "Apps", "Whiteboard", "Draw, write, and organize ideas on a shared canvas."),
            new StorePreviewProduct("9NCBCSZSJRSB", "Spotify – Music and Podcasts", "Apps", "Spotify", "Listen to music and podcasts."),
            new StorePreviewProduct("9WZDNCRFHVJL", "OneNote for Windows 10 (Legacy)", "Apps", "OneNote", "Organize notes into notebooks, sections, and pages.")
        };
        private StorePreviewProduct[] m_highlights;

        private readonly UISettings m_uiSettings = new UISettings();
        private readonly Storyboard m_pageEntrance;
        private readonly DoubleAnimation m_pageSlide;
        private bool m_motionConfigured;
        private Canvas m_workspace;
        private Rect m_restoreBounds;
        private Rect m_pointerStartBounds;
        private Point m_pointerStart;
        private UIElement m_pointerGrip;
        private uint m_pointerId;
        private bool m_maximized;
        private const double PosterWidth = 400;
        private const double PosterSpacing = 12;
        private bool m_syncingPager;
        private bool? m_wideHeader;
        private StorePreviewProduct m_product;
        private readonly DispatcherTimer m_previewTimer = new DispatcherTimer();
        private FrameworkElement m_previewAnchor;
        private bool m_dismissPreview;
        private bool m_returnToPoster;
        private double m_catalogScrollOffset;
        private double m_posterScrollOffset;
        private string m_query;
        private bool m_installing;
        private string m_searchMessage = string.Empty;
        private string m_operationId;
        private GridView m_returnGrid;
        private readonly Dictionary<(string Media, string Ranking), CatalogCollection> m_collections =
            new Dictionary<(string, string), CatalogCollection>();

        private sealed class CatalogCollection
        {
            internal readonly ObservableCollection<StorePreviewProduct> Items = new ObservableCollection<StorePreviewProduct>();
            internal readonly HashSet<string> Ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            internal int? NextPage = 1;
            internal StorePreviewProduct[] Preview;
            internal bool IsLoading;
            internal bool Failed;
        }
        private StorePreviewProduct[] m_results = Array.Empty<StorePreviewProduct>();
        private CancellationTokenSource m_searchRequest;
        private CancellationTokenSource m_suggestionRequest;
        private CancellationTokenSource m_downloadCancellation;
        private bool m_canCancelDownload;
        private bool m_installFailed;
        private Uri[] m_displayedScreenshots;
        private readonly Dictionary<string, JObject> m_packages = new Dictionary<string, JObject>();
        private readonly Dictionary<string, StorePreviewProduct> m_productDetails = new Dictionary<string, StorePreviewProduct>();
        private readonly Dictionary<string, StorePreviewProduct> m_installed = new Dictionary<string, StorePreviewProduct>();
        private readonly HashSet<string> m_installedWithoutRepack = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private int m_detailRequest;
        internal event Action Hiding;

        public StoreWindow()
        {
            InitializeComponent();
            var pageOffset = new TranslateTransform();
            PageContent.RenderTransform = pageOffset;
            var fade = new DoubleAnimation { From = 0, To = 1, Duration = TimeSpan.FromMilliseconds(220) };
            m_pageSlide = new DoubleAnimation
            {
                From = 12,
                To = 0,
                Duration = TimeSpan.FromMilliseconds(300),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };
            Storyboard.SetTarget(fade, PageContent);
            Storyboard.SetTargetProperty(fade, "Opacity");
            Storyboard.SetTarget(m_pageSlide, pageOffset);
            Storyboard.SetTargetProperty(m_pageSlide, "Y");
            m_pageEntrance = new Storyboard();
            m_pageEntrance.Children.Add(fade);
            m_pageEntrance.Children.Add(m_pageSlide);

            m_highlights = m_offlineHighlights;
            Posters.ItemsSource = m_highlights;
            FeaturePager.NumberOfPages = m_highlights.Length;
            HomeApps.ItemsSource = m_offlineHighlights.Where(p => p.Category == "Apps").ToArray();
            HomeGames.ItemsSource = m_offlineHighlights.Where(p => p.Category == "Games").ToArray();
            Sections.SelectedItem = HomeItem;
            RenderPage(false);

            // Subscribe after XAML has connected the fields and the initial page has been set.
            CatalogSearch.TextChanged += CatalogSearch_TextChanged;
            RepackAndSignOption.Checked += RepackOptionChanged;
            RepackAndSignOption.Unchecked += RepackOptionChanged;
            CatalogRanking.SelectionChanged += CatalogRanking_SelectionChanged;
            Sections.ItemInvoked += Sections_ItemInvoked;
            PageScroll.ViewChanged += PageScroll_ViewChanged;
            PosterScroll.ViewChanged += PosterScroll_ViewChanged;
            PosterScroll.SizeChanged += PosterScroll_SizeChanged;
            FeaturePager.SelectedIndexChanged += FeaturePager_SelectedIndexChanged;
            Loaded += StoreWindow_Loaded;
            Unloaded += StoreWindow_Unloaded;
            SizeChanged += StoreWindow_SizeChanged;
            m_previewTimer.Tick += PreviewTimer_Tick;
            WindowRoot.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((s, e) =>
            {
                m_previewTimer.Stop();
                AppState.Instance.ActivateStore();
            }), true);
        }

        internal void Open()
        {
            bool firstOpen = !AppState.Instance.IsStoreOpen;
            Visibility = Visibility.Visible;
            AppState.Instance.SetStoreWindowState(true, false);
            AppState.Instance.ActivateStore();
            if (firstOpen && m_workspace != null) PlayPageEntrance();
            EnsureCatalogLoaded();
        }

        internal void Close()
        {
            Hide(false);
            m_searchRequest?.Cancel();
            m_suggestionRequest?.Cancel();
            ++m_detailRequest;
            PortalPassword.Password = string.Empty;
            UpdatePortalPassword.Password = string.Empty;
            m_product = null;
            m_query = null;
            CatalogSearch.Text = string.Empty;
            FeaturePager.SelectedPageIndex = 0;
            Sections.SelectedItem = HomeItem;
            RenderPage(false);
        }

        private void Hide(bool minimized)
        {
            ClearPreview();
            SetArrowsVisible(false);
            Hiding?.Invoke();
            m_pointerGrip?.ReleasePointerCaptures();
            m_pageEntrance.Stop();
            Visibility = Visibility.Collapsed;
            AppState.Instance.SetStoreWindowState(minimized, minimized);
        }

        private void StoreWindow_Loaded(object sender, RoutedEventArgs e)
        {
            m_workspace = WindowResize.CoordinateSpace(this) as Canvas;
            if (m_workspace == null) return;
            m_workspace.SizeChanged += Workspace_SizeChanged;
            FitWorkspace();
            if (!m_motionConfigured)
            {
                m_motionConfigured = true;
                try
                {
                    ConfigureHeaderMotion();
                    ConfigureArrowMotion(PreviousPoster, -8);
                    ConfigureArrowMotion(NextPoster, 8);
                }
                catch (Exception error)
                {
                    ShellDiagnostics.RecordException("Store header and carousel animations", error);
                }
            }
            if (Visibility == Visibility.Visible) PlayPageEntrance();
        }

        private void StoreWindow_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            ClearPreview();
            bool wide = e.NewSize.Width >= 850;
            if (m_wideHeader == wide) return;
            m_wideHeader = wide;
            StoreTitle.Visibility = wide ? Visibility.Visible : Visibility.Collapsed;
            Grid.SetColumn(CatalogSearch, wide ? 0 : 1);
            Grid.SetColumnSpan(CatalogSearch, wide ? 2 : 1);
            CatalogSearch.Margin = wide ? new Thickness(260, 0, 0, 0) : new Thickness(0, 0, 8, 0);
            HeaderGap.Width = new GridLength(wide ? 40 : 0);
            CaptionColumn.Width = new GridLength(wide ? 220 : 138);
            ProfileIcon.Visibility = wide ? Visibility.Visible : Visibility.Collapsed;
            UpdatePagePadding();
        }

        private void UpdatePagePadding()
        {
            double side = m_wideHeader == false ? 16 : 36;
            PageContent.Padding = new Thickness(side, m_product == null ? 24 : 0, side, 24);
        }

        private void ConfigureHeaderMotion()
        {
            if (!m_uiSettings.AnimationsEnabled) return;
            var compositor = ElementCompositionPreview.GetElementVisual(WindowRoot).Compositor;
            ElementCompositionPreview.GetElementVisual(BackButton).CenterPoint = new Vector3(16, 16, 0);
            SetHeaderMotion(compositor, BackButton, true, true, 300);
            SetHeaderMotion(compositor, BackButton, true, false, 200);
            ElementCompositionPreview.SetIsTranslationEnabled(StoreTitle, true);
            SetHeaderMotion(compositor, StoreTitle, false, true, 300);
            SetHeaderMotion(compositor, StoreTitle, false, false, 150);
        }

        private void RailIcon_Loaded(object sender, RoutedEventArgs e)
        {
            if (!m_uiSettings.AnimationsEnabled) return;
            var visual = ElementCompositionPreview.GetElementVisual((UIElement)sender);
            var motion = visual.Compositor.CreateVector3KeyFrameAnimation();
            motion.Target = "Offset";
            motion.Duration = TimeSpan.FromMilliseconds(400);
            motion.InsertExpressionKeyFrame(1, "this.FinalValue");
            var animations = visual.Compositor.CreateImplicitAnimationCollection();
            animations["Offset"] = motion;
            visual.ImplicitAnimations = animations;
        }

        private void PressMotion_Loaded(object sender, RoutedEventArgs e)
        {
            var button = (ButtonBase)sender;
            long token = button.RegisterPropertyChangedCallback(ButtonBase.IsPressedProperty, AnimateButtonPress);
            button.Unloaded += OnUnloaded;

            void OnUnloaded(object control, RoutedEventArgs args)
            {
                button.Unloaded -= OnUnloaded;
                button.UnregisterPropertyChangedCallback(ButtonBase.IsPressedProperty, token);
            }
        }

        private void AnimateButtonPress(DependencyObject sender, DependencyProperty property)
        {
            var button = (ButtonBase)sender;
            var visual = ElementCompositionPreview.GetElementVisual(button);
            if (!m_uiSettings.AnimationsEnabled)
            {
                visual.StopAnimation("Scale");
                visual.Scale = Vector3.One;
                return;
            }

            visual.CenterPoint = new Vector3((float)button.ActualWidth / 2, (float)button.ActualHeight / 2, 0);
            bool pressed = button.IsPressed;
            float scale = pressed ? (button.ActualWidth >= 220 ? 0.985f : 0.96f) : 1f;
            var easing = visual.Compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0), new Vector2(0, 1));
            var motion = visual.Compositor.CreateVector3KeyFrameAnimation();
            motion.Duration = TimeSpan.FromMilliseconds(pressed ? 80 : 160);
            motion.InsertKeyFrame(1, new Vector3(scale, scale, 1), easing);
            visual.StartAnimation("Scale", motion);
        }

        private void AnimateOfferReveal(UIElement element, float offset)
        {
            ElementCompositionPreview.SetIsTranslationEnabled(element, true);
            var visual = ElementCompositionPreview.GetElementVisual(element);
            var compositor = visual.Compositor;
            var easing = compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0), new Vector2(0, 1));
            var fade = compositor.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0, 0);
            fade.InsertKeyFrame(1, 1, easing);
            fade.Duration = TimeSpan.FromMilliseconds(140);
            var slide = compositor.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(0, new Vector3(0, offset, 0));
            slide.InsertKeyFrame(1, Vector3.Zero, easing);
            slide.Duration = TimeSpan.FromMilliseconds(200);
            visual.StartAnimation("Opacity", fade);
            visual.Properties.StartAnimation("Translation", slide);
        }

        private static void SetHeaderMotion(Compositor compositor, UIElement element, bool scale, bool show, int milliseconds)
        {
            var duration = TimeSpan.FromMilliseconds(milliseconds);
            var opacity = compositor.CreateScalarKeyFrameAnimation();
            opacity.Target = "Opacity";
            opacity.InsertKeyFrame(0, show ? 0f : 1f);
            opacity.InsertKeyFrame(1, show ? 1f : 0f);
            opacity.Duration = duration;
            var motion = compositor.CreateVector3KeyFrameAnimation();
            motion.Target = scale ? "Scale" : "Translation";
            var rest = scale ? Vector3.One : Vector3.Zero;
            var hidden = scale ? new Vector3(0.5f, 0.5f, 1f) : new Vector3(show ? -24 : -12, 0, 0);
            motion.InsertKeyFrame(0, show ? hidden : rest);
            motion.InsertKeyFrame(1, show ? rest : hidden);
            motion.Duration = duration;
            var group = compositor.CreateAnimationGroup();
            group.Add(opacity);
            group.Add(motion);
            if (show) ElementCompositionPreview.SetImplicitShowAnimation(element, group);
            else ElementCompositionPreview.SetImplicitHideAnimation(element, group);
        }

        private void StoreWindow_Unloaded(object sender, RoutedEventArgs e)
        {
            ClearPreview();
            if (m_workspace != null) m_workspace.SizeChanged -= Workspace_SizeChanged;
            m_workspace = null;
        }

        private void Close_Click(object sender, RoutedEventArgs e) => Close();

        private void Minimize_Click(object sender, RoutedEventArgs e) => Hide(true);

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            if (m_workspace == null) return;
            if (!m_maximized) m_restoreBounds = GetBounds();
            m_maximized = !m_maximized;
            if (!m_maximized) SetBounds(m_restoreBounds);
            FitWorkspace();
            MaximizeGlyph.Glyph = m_maximized ? "\uE923" : "\uE922";
            AutomationProperties.SetName(MaximizeButton, m_maximized ? "Restore" : "Maximize");
        }

        private Rect GetBounds() => new Rect(Canvas.GetLeft(this), Canvas.GetTop(this), Width, Height);

        private void SetBounds(Rect bounds)
        {
            Canvas.SetLeft(this, bounds.X);
            Canvas.SetTop(this, bounds.Y);
            Width = bounds.Width;
            Height = bounds.Height;
        }

        private void Workspace_SizeChanged(object sender, SizeChangedEventArgs e) => FitWorkspace();

        private void FitWorkspace()
        {
            double availableWidth = m_workspace.ActualWidth;
            double availableHeight = m_workspace.ActualHeight;
            if (availableWidth <= 0 || availableHeight <= 0) return;
            if (m_maximized)
                SetBounds(new Rect(0, 0, availableWidth, availableHeight));
            else
            {
                Rect bounds = GetBounds();
                double width = Math.Min(bounds.Width, availableWidth);
                double height = Math.Min(bounds.Height, availableHeight);
                SetBounds(new Rect(Math.Max(0, Math.Min(bounds.X, availableWidth - width)),
                    Math.Max(0, Math.Min(bounds.Y, availableHeight - height)), width, height));
            }
        }

        private void Grip_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (m_maximized || m_workspace == null || m_pointerGrip != null || !WindowResize.TryCapture(sender, e)) return;
            m_pointerGrip = (UIElement)sender;
            m_pointerId = e.Pointer.PointerId;
            m_pointerStart = e.GetCurrentPoint(m_workspace).Position;
            m_pointerStartBounds = GetBounds();
            e.Handled = true;
        }

        private void Grip_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (sender != m_pointerGrip || e.Pointer.PointerId != m_pointerId || m_maximized) return;
            Point position = e.GetCurrentPoint(m_workspace).Position;
            Rect bounds = m_pointerStartBounds;
            if (((FrameworkElement)sender).Tag != null)
            {
                bounds = WindowResize.Calculate(WindowResizeCorner.BottomRight, m_pointerStart,
                    position, bounds, MinimumWidth, MinimumHeight);
                bounds.Width = Math.Min(bounds.Width, Math.Max(1, m_workspace.ActualWidth - bounds.X));
                bounds.Height = Math.Min(bounds.Height, Math.Max(1, m_workspace.ActualHeight - bounds.Y));
            }
            else
            {
                bounds.X = Math.Max(0, Math.Min(bounds.X + position.X - m_pointerStart.X, m_workspace.ActualWidth - bounds.Width));
                bounds.Y = Math.Max(0, Math.Min(bounds.Y + position.Y - m_pointerStart.Y, m_workspace.ActualHeight - bounds.Height));
            }
            SetBounds(bounds);
        }

        private void Grip_PointerEnded(object sender, PointerRoutedEventArgs e)
        {
            if (sender != m_pointerGrip || e.Pointer.PointerId != m_pointerId) return;
            m_pointerGrip = null;
            ((UIElement)sender).ReleasePointerCapture(e.Pointer);
        }

        private void Sections_ItemInvoked(muxc.NavigationView sender, muxc.NavigationViewItemInvokedEventArgs e)
        {
            if (e.InvokedItemContainer is muxc.NavigationViewItem item) ShowSection(item);
        }

        private void ShowSection(muxc.NavigationViewItem item)
        {
            Sections.SelectedItem = item;
            m_searchRequest?.Cancel();
            m_product = null;
            m_query = null;
            CatalogSearch.Text = string.Empty;
            RenderPage();
            EnsureCatalogLoaded();
        }

        private async void CatalogSearch_QuerySubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs args)
        {
            m_suggestionRequest?.Cancel();
            m_product = null;
            string query = args.ChosenSuggestion as string ?? args.QueryText;
            m_query = string.IsNullOrWhiteSpace(query) ? null : query.Trim();
            m_searchRequest?.Cancel();
            m_results = Array.Empty<StorePreviewProduct>();
            m_searchMessage = "Searching…";
            RenderPage();
            if (m_query == null) return;
            var request = new CancellationTokenSource();
            m_searchRequest = request;
            try
            {
                var results = await StoreCatalog.SearchAsync(m_query, request.Token);
                if (request.IsCancellationRequested) return;
                m_results = results;
                m_searchMessage = results.Length == 0 ? "No results found." : string.Empty;
            }
            catch (Exception error) { if (!request.IsCancellationRequested) m_searchMessage = "Search failed. " + error.Message; }
            finally
            {
                if (m_searchRequest == request) m_searchRequest = null;
                request.Dispose();
            }
            if (m_query != null && m_product == null) RenderPage(false);
        }

        private string SelectedSection => (Sections.SelectedItem as muxc.NavigationViewItem)?.Tag as string ?? "Home";
        private string SelectedRanking => (CatalogRanking.SelectedItem as ComboBoxItem)?.Tag as string ?? "TopFree";
        private static string MediaFor(string section) => section == "Gaming" ? "games" : "apps";

        private CatalogCollection Collection(string media, string ranking)
        {
            var key = (media, ranking);
            if (!m_collections.TryGetValue(key, out var collection))
                m_collections[key] = collection = new CatalogCollection();
            return collection;
        }

        private void EnsureCatalogLoaded()
        {
            if (m_product != null || m_query != null) return;
            string section = SelectedSection;
            if (section == "Home")
            {
                LoadFirstPage("apps", "TopFree");
                LoadFirstPage("games", "TopFree");
            }
            else if (section == "Apps" || section == "Gaming")
                LoadFirstPage(MediaFor(section), SelectedRanking);
        }

        private void LoadFirstPage(string media, string ranking)
        {
            if (Collection(media, ranking).NextPage == 1)
                _ = LoadCollectionAsync(media, ranking);
        }

        private async Task LoadCollectionAsync(string media, string ranking)
        {
            var collection = Collection(media, ranking);
            if (collection.IsLoading || !collection.NextPage.HasValue) return;
            collection.IsLoading = true;
            collection.Failed = false;
            bool IsCurrentPage() => m_product == null && m_query == null &&
                (SelectedSection == "Home" && ranking == "TopFree" ||
                 (SelectedSection == "Apps" || SelectedSection == "Gaming") &&
                 MediaFor(SelectedSection) == media && SelectedRanking == ranking);
            if (IsCurrentPage()) RenderPage(false, resetScroll: false);
            try
            {
                bool firstPage = collection.NextPage == 1;
                var page = await StoreCatalog.CollectionAsync(media, ranking, collection.NextPage.Value);
                foreach (var item in page.Items)
                    if (collection.Ids.Add(item.Id)) collection.Items.Add(item);
                collection.NextPage = page.NextPage;
                if (firstPage && ranking == "TopFree")
                {
                    collection.Preview = collection.Items.Take(6).ToArray();
                    UpdateHighlights();
                }
            }
            catch (Exception error)
            {
                collection.Failed = true;
                ShellDiagnostics.RecordException("Loading a Store collection", error);
            }
            finally { collection.IsLoading = false; }
            if (IsCurrentPage()) RenderPage(false, resetScroll: false);
        }

        private void UpdateHighlights()
        {
            var apps = Collection("apps", "TopFree").Items;
            var games = Collection("games", "TopFree").Items;
            var featured = apps.Take(3).Concat(games.Take(3)).ToArray();
            if (featured.Length == 0) return;

            m_highlights = featured;
            m_syncingPager = true;
            try
            {
                Posters.ItemsSource = featured;
                FeaturePager.NumberOfPages = featured.Length;
                FeaturePager.SelectedPageIndex = 0;
            }
            finally { m_syncingPager = false; }
            CenterPoster(true);
        }

        private void SeeApps_Click(object sender, RoutedEventArgs e) => SeeCollection(AppsItem);
        private void SeeGames_Click(object sender, RoutedEventArgs e) => SeeCollection(GamingItem);

        private void SeeCollection(muxc.NavigationViewItem item)
        {
            CatalogRanking.SelectedIndex = 0;
            ShowSection(item);
        }

        private void CatalogRanking_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            RenderPage();
            EnsureCatalogLoaded();
        }

        private void MoreCatalogItems_Click(object sender, RoutedEventArgs e)
        {
            if (SelectedSection == "Home") EnsureCatalogLoaded();
            else _ = LoadCollectionAsync(MediaFor(SelectedSection), SelectedRanking);
        }

        private async void CatalogSearch_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
        {
            m_suggestionRequest?.Cancel();
            if (args.Reason != AutoSuggestionBoxTextChangeReason.UserInput)
            {
                if (string.IsNullOrEmpty(sender.Text)) sender.ItemsSource = null;
                return;
            }

            string input = sender.Text.Trim();
            sender.ItemsSource = null;
            if (input.Length == 0 && m_query != null)
            {
                m_searchRequest?.Cancel();
                m_product = null;
                m_query = null;
                RenderPage();
                EnsureCatalogLoaded();
            }
            if (input.Length < 2) return;
            var request = new CancellationTokenSource();
            m_suggestionRequest = request;
            try
            {
                await Task.Delay(300, request.Token);
                var suggestions = await StoreCatalog.SuggestionsAsync(input, request.Token);
                if (!request.IsCancellationRequested) sender.ItemsSource = suggestions;
            }
            catch (Exception)
            {
                // Suggestions are optional; submitting the text still performs a full search.
                if (!request.IsCancellationRequested) sender.ItemsSource = null;
            }
            finally
            {
                if (m_suggestionRequest == request) m_suggestionRequest = null;
                request.Dispose();
            }
        }

        private void Back_Click(object sender, RoutedEventArgs e)
        {
            var product = m_product;
            if (product != null)
            {
                var motion = PrepareCardMotion(m_returnToPoster ? DetailCover : DetailIcon);
                var titleMotion = PrepareCardMotion(DetailTitle, "StoreProductTitle");
                m_product = null;
                RenderPage(animate: false);
                PageScroll.UpdateLayout();
                PageScroll.ChangeView(null, m_catalogScrollOffset, null, true);
                if (m_returnToPoster) PosterScroll.ChangeView(m_posterScrollOffset, null, null, true);
                WindowRoot.UpdateLayout();
                var container = m_returnToPoster
                    ? Posters.ContainerFromItem(product) as FrameworkElement
                    : m_returnGrid?.ContainerFromItem(product) as FrameworkElement;
                var target = FindCardArt(container, m_returnToPoster);
                var focusTarget = m_returnToPoster ? FindCardPart<Button>(container) : container as Control;
                (focusTarget ?? Sections.SelectedItem as Control)?.Focus(FocusState.Programmatic);
                StartCardMotion(titleMotion, FindCardPart<TextBlock>(container, "CardTitle"));
                if (!StartCardMotion(motion, target)) PlayPageEntrance(true);
                return;
            }
            m_query = null;
            CatalogSearch.Text = string.Empty;
            RenderPage(backward: true);
            EnsureCatalogLoaded();
            if (!BackButton.IsEnabled && Sections.SelectedItem is muxc.NavigationViewItem item)
                item.Focus(FocusState.Programmatic);
        }

        private void Products_ItemClick(object sender, ItemClickEventArgs e)
        {
            if (e.ClickedItem is StorePreviewProduct product)
            {
                m_returnGrid = (GridView)sender;
                OpenProduct(product, m_returnGrid.ContainerFromItem(product) as FrameworkElement, false);
            }
        }

        private void Poster_Click(object sender, RoutedEventArgs e)
        {
            var button = (Button)sender;
            if (button.DataContext is StorePreviewProduct product)
                OpenProduct(product, button, true);
        }

        private static T FindCardPart<T>(DependencyObject root, string name = null) where T : FrameworkElement
        {
            if (root == null) return null;
            if (root is T part && (name == null || part.Name == name)) return part;
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            {
                var match = FindCardPart<T>(VisualTreeHelper.GetChild(root, i), name);
                if (match != null) return match;
            }
            return null;
        }

        private static Image FindCardArt(FrameworkElement container, bool poster) =>
            FindCardPart<Image>(container, poster ? "PosterArt" : "CardTile");

        private ConnectedAnimation PrepareCardMotion(FrameworkElement source, string key = "StoreProductArtwork")
        {
            if (!m_uiSettings.AnimationsEnabled || source == null || source.ActualWidth <= 0) return null;
            var motion = ConnectedAnimationService.GetForCurrentView().PrepareToAnimate(key, source);
            motion.Configuration = new DirectConnectedAnimationConfiguration();
            return motion;
        }

        private static bool StartCardMotion(ConnectedAnimation motion, FrameworkElement destination)
        {
            if (motion == null) return false;
            if (destination != null && destination.ActualWidth > 0 && motion.TryStart(destination)) return true;
            motion.Cancel();
            return false;
        }

        private void OpenProduct(StorePreviewProduct product, FrameworkElement source, bool poster)
        {
            var motion = PrepareCardMotion(source == CardPreview ? (poster ? PreviewPoster : PreviewTile) : FindCardArt(source, poster));
            var titleMotion = PrepareCardMotion(source == CardPreview ? PreviewTitle : FindCardPart<TextBlock>(source, "CardTitle"), "StoreProductTitle");
            m_catalogScrollOffset = PageScroll.VerticalOffset;
            m_posterScrollOffset = PosterScroll.HorizontalOffset;
            m_returnToPoster = poster;
            m_product = product;
            RepackAndSignOption.IsChecked = false;
            RenderPage(animate: false);
            WindowRoot.UpdateLayout();
            StartCardMotion(titleMotion, DetailTitle);
            if (!StartCardMotion(motion, poster ? DetailCover : DetailIcon)) PlayPageEntrance();
            BackButton.Focus(FocusState.Programmatic);
            _ = LoadOfferAsync(product);
        }

        private async Task LoadOfferAsync(StorePreviewProduct product)
        {
            int request = ++m_detailRequest;
            UpdateOffer();
            try
            {
                var records = await StoreCatalog.ProductsAsync(product.Id, CancellationToken.None);
                await RepackedAppRegistry.InitializeAsync();
                if (request != m_detailRequest || m_product != product) return;
                var record = records.FirstOrDefault(p => (string)p["ProductId"] == product.Id);
                m_packages[product.Id] = record == null ? null : StoreCatalog.FindPackage(record);
                var details = record == null ? null : StoreCatalog.Card(record);
                if (details != null) m_productDetails[product.Id] = details;
                if (request == m_detailRequest && m_product == product)
                {
                    ShowProductMetadata(product);
                    UpdateOffer();
                }
            }
            catch (Exception error)
            {
                if (request == m_detailRequest && m_product == product) GetStatus.Text = "Could not check availability. " + error.Message;
            }
        }

        private void RepackOptionChanged(object sender, RoutedEventArgs e) => UpdateOffer();

        private static Version GetInstalledVersion(JObject package, bool repacked, out Version offeredVersion)
        {
            var parts = ((string)package?["PackageFullName"])?.Split('_');
            offeredVersion = null;
            if (parts == null || parts.Length < 2 || !Version.TryParse(parts[1], out offeredVersion)) return null;
            return RepackedAppRegistry.InstalledVersion(parts[0], repacked);
        }

        private void UpdateOffer()
        {
            CancelFromDownloads.Visibility = m_canCancelDownload ? Visibility.Visible : Visibility.Collapsed;
            if (m_product == null) return;
            bool selectedOperation = m_operationId == m_product.Id;
            bool installing = m_installing && selectedOperation;
            bool checkedOffer = m_packages.TryGetValue(m_product.Id, out var package);
            bool repack = RepackAndSignOption.IsChecked == true;
            var installedVersion = GetInstalledVersion(package, repack, out var offeredVersion);
            bool update = installedVersion != null && offeredVersion > installedVersion;
            bool installed = installedVersion != null ? !update :
                !repack && m_installedWithoutRepack.Contains(m_product.Id);
            GetButton.Content = installed ? "Installed" : selectedOperation && m_installFailed ? "Try again" : update ? "Update" : "Get";
            bool hadActionFocus = GetButton.FocusState != FocusState.Unfocused ||
                CancelDownloadButton.FocusState != FocusState.Unfocused || GetProgress.FocusState != FocusState.Unfocused;
            GetButton.IsEnabled = package != null && !m_installing && !installed;
            RepackAndSignOption.IsEnabled = package != null && !m_installing;

            bool showProgress = GetProgressPanel.Visibility == Visibility.Visible;
            GetButton.Visibility = installing ? Visibility.Collapsed : Visibility.Visible;
            GetProgressPanel.Visibility = installing ? Visibility.Visible : Visibility.Collapsed;
            CancelDownloadButton.Visibility = installing && m_canCancelDownload ? Visibility.Visible : Visibility.Collapsed;
            GetProgressText.Text = installing ? DownloadStatus.Text : string.Empty;
            if (installing) GetStatus.Text = string.Empty;
            else if (update && !(selectedOperation && m_installFailed)) GetStatus.Text = "Update available.";
            else if (selectedOperation) GetStatus.Text = DownloadStatus.Text;
            else if (installed) GetStatus.Text = "Installed";
            else if (!checkedOffer) GetStatus.Text = "Checking availability…";
            else if (package == null) GetStatus.Text = "No compatible free package is available for this device.";
            else GetStatus.Text = m_installing ? "Another installation is in progress." : string.Empty;

            if (hadActionFocus && ProductDetails.Visibility == Visibility.Visible)
            {
                if (installing)
                {
                    var target = m_canCancelDownload ? (Control)CancelDownloadButton : GetProgress;
                    if (target.FocusState == FocusState.Unfocused) target.Focus(FocusState.Programmatic);
                }
                else if (GetButton.IsEnabled) GetButton.Focus(FocusState.Programmatic);
                else if (showProgress) BackButton.Focus(FocusState.Programmatic);
            }

            if (installing != showProgress && m_uiSettings.AnimationsEnabled)
                AnimateOfferReveal(installing ? (UIElement)GetProgressPanel : GetButton, installing ? 6 : -4);
        }

        private void ShowProductMetadata(StorePreviewProduct product)
        {
            var details = m_productDetails.TryGetValue(product.Id, out var catalogProduct) ? catalogProduct : product;
            DetailDescription.Text = details.Description;
            DetailPublisher.Text = details.Publisher ?? string.Empty;
            DetailPublisher.Visibility = string.IsNullOrWhiteSpace(details.Publisher) ? Visibility.Collapsed : Visibility.Visible;
            var screenshots = details.Screenshots;
            var section = ScreenshotSection;
            if (section == null && screenshots.Length > 0)
                section = FindName(nameof(ScreenshotSection)) as StackPanel;
            if (section == null) return;
            if (m_displayedScreenshots != screenshots)
            {
                m_displayedScreenshots = screenshots;
                ProductScreenshots.ItemsSource = screenshots.Select(uri => new BitmapImage
                {
                    DecodePixelWidth = 840,
                    UriSource = uri
                }).ToArray();
                if (screenshots.Length > 0) ProductScreenshots.SelectedIndex = 0;
            }
            section.Visibility = screenshots.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private void CancelDownload_Click(object sender, RoutedEventArgs e)
        {
            if (!m_canCancelDownload) return;
            m_canCancelDownload = false;
            DownloadStatus.Text = "Canceling download…";
            m_downloadCancellation.Cancel();
            UpdateOffer();
        }

        private async void Get_Click(object sender, RoutedEventArgs e)
        {
            var product = m_product;
            if (product == null || m_installing || !m_packages.TryGetValue(product.Id, out var package) || package == null) return;
            bool repackAndSign = RepackAndSignOption.IsChecked == true;
            var installedVersion = GetInstalledVersion(package, repackAndSign, out var offeredVersion);
            bool update = installedVersion != null && offeredVersion > installedVersion;
            await InstallStorePackageAsync(product, package, repackAndSign, update, PortalUser.Text,
                PortalPassword.Password, TrustLocal.IsChecked == true);
        }

        private readonly ObservableCollection<StoreAppUpdate> m_availableUpdates = new ObservableCollection<StoreAppUpdate>();

        private async void CheckUpdates_Click(object sender, RoutedEventArgs e)
        {
            if (m_installing || !CheckUpdates.IsEnabled) return;
            CheckUpdates.IsEnabled = false;
            m_availableUpdates.Clear();
            AvailableUpdates.ItemsSource = m_availableUpdates;
            UpdateCheckStatus.Text = "Checking for updates…";
            try
            {
                await RepackedAppRegistry.InitializeAsync();
                var registered = RepackedAppRegistry.GetInstalledPackages();
                var families = registered.ToDictionary(item => (item.Name, item.Repacked), item => item.Family);
                if (registered.Any(item => !item.Repacked && string.IsNullOrEmpty(item.Family)))
                {
                    try
                    {
                        var installed = new Windows.Management.Deployment.PackageManager().FindPackagesForUser(string.Empty)
                            .GroupBy(p => p.Id.Name, StringComparer.OrdinalIgnoreCase)
                            .ToDictionary(g => g.Key, g => g.Select(p => p.Id.FamilyName).Distinct().ToArray(), StringComparer.OrdinalIgnoreCase);
                        foreach (var item in registered)
                            if (!item.Repacked && string.IsNullOrEmpty(item.Family) &&
                                installed.TryGetValue(item.Name, out var matches) && matches.Length == 1)
                                families[(item.Name, item.Repacked)] = matches[0];
                    }
                    catch (Exception error)
                    {
                        ShellDiagnostics.RecordException("Resolving older Store package entries", error);
                    }
                }
                var unresolved = 0;
                using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(45)))
                {
                    foreach (var group in registered.GroupBy(item => families[(item.Name, item.Repacked)], StringComparer.OrdinalIgnoreCase))
                    {
                        if (string.IsNullOrEmpty(group.Key))
                        {
                            unresolved += group.Count();
                            continue;
                        }
                        try
                        {
                            var record = await StoreCatalog.ProductForFamilyAsync(group.Key, timeout.Token);
                            var product = record == null ? null : StoreCatalog.Card(record);
                            var package = record == null ? null : StoreCatalog.FindPackage(record, group.Key);
                            var parts = ((string)package?["PackageFullName"])?.Split('_');
                            if (product == null || parts == null || parts.Length < 2 || !Version.TryParse(parts[1], out var version))
                            {
                                unresolved += group.Count();
                                continue;
                            }
                            foreach (var item in group)
                                if (string.Equals(parts[0], item.Name, StringComparison.OrdinalIgnoreCase) && version > item.Version)
                                    m_availableUpdates.Add(new StoreAppUpdate(product, package, item.Name, item.Version, version, item.Repacked));
                        }
                        catch (Exception error)
                        {
                            unresolved += group.Count();
                            ShellDiagnostics.RecordException("Checking a Store package for updates", error);
                        }
                    }
                }
                UpdateCheckStatus.Text = unresolved > 0 ? "Some installed packages could not be checked." :
                    m_availableUpdates.Count == 0 ? "No updates found." : string.Empty;
            }
            catch (Exception error)
            {
                ShellDiagnostics.RecordException("Checking for Store updates", error);
                UpdateCheckStatus.Text = "Could not check for updates.";
            }
            finally { CheckUpdates.IsEnabled = !m_installing; }
        }

        private async void DownloadUpdate_Click(object sender, RoutedEventArgs e)
        {
            if (m_installing || !CheckUpdates.IsEnabled ||
                !((sender as FrameworkElement)?.DataContext is StoreAppUpdate update)) return;
            var installed = RepackedAppRegistry.InstalledVersion(update.IdentityName, update.Repacked);
            if (installed != null && installed >= update.Version)
            {
                m_availableUpdates.Remove(update);
                return;
            }
            if (await InstallStorePackageAsync(update.Product, update.Package, update.Repacked, true, UpdatePortalUser.Text,
                UpdatePortalPassword.Password, UpdateTrustLocal.IsChecked == true))
                m_availableUpdates.Remove(update);
        }

        private async Task<bool> InstallStorePackageAsync(StorePreviewProduct product, JObject package, bool repackAndSign,
            bool update, string username, string password, bool trustLocal)
        {
            m_installing = true;
            CheckUpdates.IsEnabled = false;
            m_canCancelDownload = true;
            m_installFailed = false;
            var cancellation = new CancellationTokenSource();
            m_downloadCancellation = cancellation;
            m_operationId = product.Id;
            DownloadName.Text = product.Name;
            DownloadStatus.Text = "Preparing download…";
            DownloadProgress.Visibility = Visibility.Visible;
            DownloadProgress.Value = 0;
            DownloadProgress.IsIndeterminate = true;
            UpdateOffer();
            bool reporting = true;
            try
            {
                var progress = new Progress<string>(message =>
                {
                    if (!reporting || cancellation.IsCancellationRequested) return;
                    DownloadStatus.Text = message;
                    int start = message.LastIndexOf(' ');
                    if (start >= 0 && message.EndsWith("%") && int.TryParse(message.Substring(start + 1).TrimEnd('%'), out int percent))
                    {
                        DownloadProgress.IsIndeterminate = false;
                        DownloadProgress.Value = percent;
                    }
                    else DownloadProgress.IsIndeterminate = true;
                    if (m_product?.Id == product.Id) GetProgressText.Text = message;
                });
                var notice = await StoreDelivery.InstallAsync(package, username, password, trustLocal, repackAndSign,
                    progress, () =>
                    {
                        m_canCancelDownload = false;
                        DownloadStatus.Text = repackAndSign ? "Preparing to repack and sign…" : "Preparing installation…";
                        DownloadProgress.IsIndeterminate = true;
                        UpdateOffer();
                    }, cancellation.Token);
                m_installed[product.Id] = product;
                if (!repackAndSign) m_installedWithoutRepack.Add(product.Id);
                reporting = false;
                DownloadStatus.Text = string.IsNullOrEmpty(notice) ? (update ? "Updated." : "Installed.") : notice;
                try { await App.ServiceProvider.GetRequiredService<AppHelper>().LoadAppsAsync(); }
                catch { DownloadStatus.Text += " Could not refresh the Start menu."; }
                return true;
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
            {
                DownloadStatus.Text = "Download canceled.";
                return false;
            }
            catch (Exception error)
            {
                m_installFailed = true;
                DownloadStatus.Text = "Installation failed. " + error.Message;
                return false;
            }
            finally
            {
                reporting = false;
                m_installing = false;
                CheckUpdates.IsEnabled = true;
                m_canCancelDownload = false;
                m_downloadCancellation = null;
                cancellation.Dispose();
                DownloadProgress.IsIndeterminate = false;
                DownloadProgress.Visibility = Visibility.Collapsed;
                if (LibraryFilters.Visibility == Visibility.Visible) RenderPage(false);
                UpdateOffer();
            }
        }

        private void Card_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
            if (e.Pointer.PointerDeviceType != PointerDeviceType.Mouse || e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            var anchor = (FrameworkElement)sender;
            if (anchor != m_previewAnchor) ClearPreview();
            m_previewAnchor = anchor;
            m_dismissPreview = false;
            m_previewTimer.Stop();
            if (CardPreview.Visibility == Visibility.Visible) return;
            m_previewTimer.Interval = TimeSpan.FromMilliseconds(450);
            m_previewTimer.Start();
        }

        private void Card_PointerExited(object sender, PointerRoutedEventArgs e)
        {
            if (e.Pointer.PointerDeviceType != PointerDeviceType.Mouse || (sender != m_previewAnchor && sender != CardPreview)) return;
            m_previewTimer.Stop();
            m_dismissPreview = true;
            if (CardPreview.Visibility != Visibility.Visible) return;
            m_previewTimer.Interval = TimeSpan.FromMilliseconds(100);
            m_previewTimer.Start();
        }

        private void Preview_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
            m_dismissPreview = false;
            m_previewTimer.Stop();
        }

        private void ClearPreview()
        {
            m_previewTimer.Stop();
            m_previewAnchor = null;
            if (CardPreview != null) CardPreview.Visibility = Visibility.Collapsed;
        }

        private void PreviewTimer_Tick(object sender, object e)
        {
            m_previewTimer.Stop();
            if (m_dismissPreview || !(m_previewAnchor?.DataContext is StorePreviewProduct product) ||
                !m_previewAnchor.IsLoaded || Visibility != Visibility.Visible || PreviewLayer.ActualWidth <= 0)
            {
                ClearPreview();
                return;
            }
            bool poster = m_previewAnchor is Button;
            PreviewPoster.Visibility = poster ? Visibility.Visible : Visibility.Collapsed;
            PreviewHeader.MinHeight = poster ? 74 : 64;
            PreviewPoster.Source = poster ? product.Cover : null;
            PreviewTile.Source = product.Tile;
            PreviewTitle.Text = product.Name;
            PreviewCategory.Text = product.Category;
            PreviewDescription.Text = product.Description;
            AutomationProperties.SetName(CardPreview, product.Name);
            double width = Math.Min(PreviewLayer.ActualWidth, Math.Max(m_previewAnchor.ActualWidth, 288));
            CardPreview.Width = width;
            CardPreview.Visibility = Visibility.Visible;
            CardPreview.Measure(new Size(width, double.PositiveInfinity));
            double height = CardPreview.DesiredSize.Height;
            if (height > PreviewLayer.ActualHeight) { ClearPreview(); return; }
            var origin = m_previewAnchor.TransformToVisual(PreviewLayer).TransformPoint(new Point());
            double left = origin.X + (m_previewAnchor.ActualWidth - width) / 2;
            double x = Math.Max(0, Math.Min(left, PreviewLayer.ActualWidth - width));
            double y = Math.Max(0, Math.Min(origin.Y, PreviewLayer.ActualHeight - height));
            Canvas.SetLeft(CardPreview, x);
            Canvas.SetTop(CardPreview, y);
            Canvas.SetLeft(ShadowHost, x);
            Canvas.SetTop(ShadowHost, y);
            UpdatePreviewShadow(width, height);
            if (m_uiSettings.AnimationsEnabled)
            {
                var visual = ElementCompositionPreview.GetElementVisual(CardPreview);
                var reveal = visual.Compositor.CreateScalarKeyFrameAnimation();
                reveal.InsertKeyFrame(0, 0);
                reveal.InsertKeyFrame(1, 1);
                reveal.Duration = TimeSpan.FromMilliseconds(150);
                visual.StartAnimation("Opacity", reveal);
            }
        }

        private void UpdatePreviewShadow(double width, double height)
        {
            ShadowHost.Width = width;
            ShadowHost.Height = height;
            var sprite = ElementCompositionPreview.GetElementChildVisual(ShadowHost) as SpriteVisual;
            if (sprite == null)
            {
                var compositor = ElementCompositionPreview.GetElementVisual(ShadowHost).Compositor;
                var shadow = compositor.CreateDropShadow();
                shadow.BlurRadius = 12;
                shadow.Offset = new Vector3(0, 4, 0);
                shadow.Mask = ShadowMask.GetAlphaMask();
                sprite = compositor.CreateSpriteVisual();
                sprite.Shadow = shadow;
                ElementCompositionPreview.SetElementChildVisual(ShadowHost, sprite);
            }
            sprite.Size = new Vector2((float)width, (float)height);
            sprite.Opacity = ActualTheme == ElementTheme.Light ? 0.06f : 0.18f;
        }

        private void Preview_Click(object sender, RoutedEventArgs e)
        {
            if (m_previewAnchor?.DataContext is StorePreviewProduct product)
            {
                bool poster = m_previewAnchor is Button;
                if (!poster)
                {
                    DependencyObject parent = m_previewAnchor;
                    while (parent != null && !(parent is GridView)) parent = VisualTreeHelper.GetParent(parent);
                    m_returnGrid = parent as GridView;
                }
                OpenProduct(product, CardPreview, poster);
            }
        }

        private void PageScroll_ViewChanged(object sender, ScrollViewerViewChangedEventArgs e) => ClearPreview();

        private void Preview_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            var point = e.GetCurrentPoint(this);
            ClearPreview();
            if (point.Properties.IsHorizontalMouseWheel)
                PosterScroll.ChangeView(PosterScroll.HorizontalOffset + point.Properties.MouseWheelDelta, null, null);
            else
                PageScroll.ChangeView(null, PageScroll.VerticalOffset - point.Properties.MouseWheelDelta, null);
            e.Handled = true;
        }

        private void WindowRoot_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Escape && CardPreview.Visibility == Visibility.Visible)
            {
                ClearPreview();
                e.Handled = true;
            }
        }

        private void ConfigureArrowMotion(Button button, float offset)
        {
            if (!m_uiSettings.AnimationsEnabled) return;
            ElementCompositionPreview.SetIsTranslationEnabled(button, true);
            var compositor = ElementCompositionPreview.GetElementVisual(button).Compositor;
            var move = compositor.CreateVector3KeyFrameAnimation();
            move.Target = "Translation";
            move.InsertKeyFrame(0, new Vector3(offset, 0, 0));
            move.InsertKeyFrame(1, Vector3.Zero);
            move.Duration = TimeSpan.FromMilliseconds(300);
            var fade = compositor.CreateScalarKeyFrameAnimation();
            fade.Target = "Opacity";
            fade.InsertKeyFrame(0, 0);
            fade.InsertKeyFrame(1, 1);
            fade.Duration = move.Duration;
            var entrance = compositor.CreateAnimationGroup();
            entrance.Add(move);
            entrance.Add(fade);
            ElementCompositionPreview.SetImplicitShowAnimation(button, entrance);
            var exit = compositor.CreateScalarKeyFrameAnimation();
            exit.Target = "Opacity";
            exit.InsertKeyFrame(0, 1);
            exit.InsertKeyFrame(1, 0);
            exit.Duration = TimeSpan.FromMilliseconds(100);
            ElementCompositionPreview.SetImplicitHideAnimation(button, exit);
        }

        private void SetArrowsVisible(bool visible)
        {
            var state = visible ? Visibility.Visible : Visibility.Collapsed;
            PreviousPoster.Visibility = NextPoster.Visibility = state;
        }

        private void Feature_PointerEntered(object sender, PointerRoutedEventArgs e) => SetArrowsVisible(true);

        private void Feature_PointerExited(object sender, PointerRoutedEventArgs e) => SetArrowsVisible(false);

        private void PreviousFeature_Click(object sender, RoutedEventArgs e) => ChangeFeature(-1);

        private void NextFeature_Click(object sender, RoutedEventArgs e) => ChangeFeature(1);

        private void ChangeFeature(int step)
        {
            FeaturePager.SelectedPageIndex = (FeaturePager.SelectedPageIndex + step + m_highlights.Length) % m_highlights.Length;
        }

        private void FeaturePager_SelectedIndexChanged(muxc.PipsPager sender, muxc.PipsPagerSelectedIndexChangedEventArgs args)
        {
            if (!m_syncingPager) CenterPoster(!m_uiSettings.AnimationsEnabled);
        }

        private void PosterScroll_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            double inset = Math.Max(0, (e.NewSize.Width - PosterWidth) / 2);
            Posters.Margin = new Thickness(inset, 0, inset, 0);
            CenterPoster(true);
        }

        private double PosterOrigin => Math.Max(0, (PosterWidth - PosterScroll.ViewportWidth) / 2);

        private void CenterPoster(bool immediate)
        {
            if (PosterScroll.ViewportWidth <= 0 || FeaturePager.SelectedPageIndex < 0) return;
            double offset = PosterOrigin + FeaturePager.SelectedPageIndex * (PosterWidth + PosterSpacing);
            PosterScroll.ChangeView(offset, null, null, immediate);
        }

        private void PosterScroll_ViewChanged(object sender, ScrollViewerViewChangedEventArgs e)
        {
            ClearPreview();
            if (e.IsIntermediate || PosterScroll.ViewportWidth <= 0 || Feature.Visibility != Visibility.Visible) return;
            int index = (int)Math.Round((PosterScroll.HorizontalOffset - PosterOrigin) / (PosterWidth + PosterSpacing));
            m_syncingPager = true;
            try { FeaturePager.SelectedPageIndex = Math.Max(0, Math.Min(index, m_highlights.Length - 1)); }
            finally { m_syncingPager = false; }
        }

        private void RenderPage(bool animate = true, bool backward = false, bool resetScroll = true)
        {
            ClearPreview();
            string section = SelectedSection;
            bool browsing = m_product == null && m_query == null;
            bool home = browsing && section == "Home";
            bool ranked = browsing && (section == "Apps" || section == "Gaming");
            Feature.Visibility = home ? Visibility.Visible : Visibility.Collapsed;
            HomeCollections.Visibility = Feature.Visibility;
            CatalogRanking.Visibility = ranked ? Visibility.Visible : Visibility.Collapsed;
            MoreCatalogItems.Visibility = Visibility.Collapsed;
            BackButton.IsEnabled = m_product != null || m_query != null;
            BackButton.Visibility = BackButton.IsEnabled ? Visibility.Visible : Visibility.Collapsed;
            ProductDetails.Visibility = m_product != null ? Visibility.Visible : Visibility.Collapsed;
            UpdatePagePadding();
            HeaderRow.Visibility = m_product == null ? Visibility.Visible : Visibility.Collapsed;
            LibraryFilters.Visibility = section == "Library" && !BackButton.IsEnabled ? Visibility.Visible : Visibility.Collapsed;
            DownloadSections.Visibility = section == "Downloads" && !BackButton.IsEnabled ? Visibility.Visible : Visibility.Collapsed;
            CheckUpdates.Visibility = DownloadSections.Visibility;

            IList<StorePreviewProduct> products = Array.Empty<StorePreviewProduct>();
            if (m_product != null)
            {
                DetailTitle.Text = m_product.Name;
                ShowProductMetadata(m_product);
                DetailCover.Source = m_product.Cover;
                PageMessage.Text = string.Empty;
                DetailCategory.Text = m_product.Category;
                DetailIcon.Source = m_product.Tile;
                UpdateOffer();
            }
            else if (m_query != null)
            {
                PageTitle.Text = "Search results";
                products = m_results;
                PageMessage.Text = m_searchMessage;
            }
            else
            {
                PageTitle.Text = home ? "Discover" : section;
                if (home)
                {
                    var apps = Collection("apps", "TopFree");
                    var games = Collection("games", "TopFree");
                    if (apps.Preview != null && HomeApps.ItemsSource != apps.Preview) HomeApps.ItemsSource = apps.Preview;
                    if (games.Preview != null && HomeGames.ItemsSource != games.Preview) HomeGames.ItemsSource = games.Preview;
                    bool loading = apps.IsLoading || games.IsLoading;
                    bool failed = apps.Failed || games.Failed;
                    PageMessage.Text = loading ? "Loading collections…" : failed ? "Some collections couldn’t load." : string.Empty;
                    if (failed && !loading)
                    {
                        MoreCatalogItems.Content = "Try again";
                        MoreCatalogItems.IsEnabled = true;
                        MoreCatalogItems.Visibility = Visibility.Visible;
                    }
                }
                else if (ranked)
                {
                    var collection = Collection(MediaFor(section), SelectedRanking);
                    products = collection.Items;
                    PageMessage.Text = collection.IsLoading && products.Count == 0 ? "Loading collection…" :
                        collection.Failed ? "Couldn’t load this collection." :
                        products.Count == 0 && !collection.NextPage.HasValue ? "No products found." : string.Empty;
                    MoreCatalogItems.Content = collection.IsLoading ? "Loading…" : collection.Failed ? "Try again" : "Load more";
                    MoreCatalogItems.IsEnabled = !collection.IsLoading;
                    MoreCatalogItems.Visibility = collection.NextPage.HasValue ? Visibility.Visible : Visibility.Collapsed;
                }
                else if (section == "Library")
                {
                    products = m_installed.Values.ToArray();
                    PageMessage.Text = products.Count == 0 ? "No apps installed in this session." : string.Empty;
                }
                else
                    PageMessage.Text = string.Empty;
            }
            PageMessage.Visibility = string.IsNullOrEmpty(PageMessage.Text) ? Visibility.Collapsed : Visibility.Visible;
            if (Products.ItemsSource != products) Products.ItemsSource = products;
            Products.Visibility = products.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
            if (resetScroll) PageScroll.ChangeView(null, 0, null, true);
            if (animate && m_workspace != null && Visibility == Visibility.Visible) PlayPageEntrance(backward);
        }

        private void PlayPageEntrance(bool backward = false)
        {
            if (!m_uiSettings.AnimationsEnabled) return;
            try
            {
                m_pageEntrance.Stop();
                m_pageSlide.From = backward ? -12 : 12;
                m_pageEntrance.Begin();
            }
            catch (Exception error)
            {
                ShellDiagnostics.RecordException("Store page transition", error);
            }
        }
    }

    public sealed class StoreAppUpdate
    {
        internal StoreAppUpdate(StorePreviewProduct product, JObject package, string identityName, Version installed, Version version, bool repacked)
        {
            Product = product;
            Package = package;
            IdentityName = identityName;
            Version = version;
            Repacked = repacked;
            VersionLabel = installed + " → " + version + (repacked ? " · Repack" : "");
        }

        internal StorePreviewProduct Product { get; }
        internal JObject Package { get; }
        internal string IdentityName { get; }
        internal Version Version { get; }
        internal bool Repacked { get; }
        public string Name => Product.Name;
        public BitmapImage Tile => Product.Tile;
        public string VersionLabel { get; }
    }

    public sealed class StorePreviewProduct
    {
        private readonly Uri m_coverUri;
        private BitmapImage m_cover;

        internal StorePreviewProduct(string id, string name, string category, string asset, string description)
            : this(id, name, category, description, new Uri("ms-appx:///Assets/Store/" + asset + "Tile.png"),
                new Uri("ms-appx:///Assets/Store/" + asset + "Cover.jpg")) { }

        internal StorePreviewProduct(string id, string name, string category, string description, Uri tile, Uri cover)
        {
            Id = id;
            Name = name;
            Category = category;
            Description = description;
            Tile = new BitmapImage { DecodePixelWidth = 150, UriSource = tile };
            m_coverUri = cover;
        }

        internal string Id { get; }
        internal string Publisher { get; set; }
        internal Uri[] Screenshots { get; set; } = Array.Empty<Uri>();

        public string Name { get; }
        public string Category { get; }
        public BitmapImage Tile { get; }
        public BitmapImage Cover => m_cover ?? (m_cover = new BitmapImage { DecodePixelWidth = 800, UriSource = m_coverUri });
        public string Description { get; }
        public string Price { get; internal set; } = string.Empty;
    }
}
