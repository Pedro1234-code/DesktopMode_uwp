using factoryos_10x_shell.Library.Models.InternalData;
using System.Collections.ObjectModel;
using System;
using System.Threading.Tasks;
using Windows.Storage;

namespace factoryos_10x_shell.Library.Services.WebApps
{
    public interface IWebAppService
    {
        ObservableCollection<WebAppDefinition> InstalledApps { get; }
        event EventHandler AppsChanged;
        Task InitializeAsync();
        Task<WebAppDefinition> InstallAsync(string name, string startUri, bool useStrawfox = false, bool useStrawfoxWindow = false, byte[] favicon = null);
        void SetPinned(WebAppDefinition app, bool isPinned);
        void Uninstall(WebAppDefinition app);
        Task ChangeIconAsync(WebAppDefinition app, StorageFile file);
        Task SaveFaviconAsync(WebAppDefinition app, string faviconUri);
    }
}
