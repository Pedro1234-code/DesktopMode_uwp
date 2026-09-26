using factoryos_10x_shell.Library.Models.InternalData;
using System.Collections.ObjectModel;
using System;
using System.Threading.Tasks;

namespace factoryos_10x_shell.Library.Services.WebApps
{
    public interface IWebAppService
    {
        ObservableCollection<WebAppDefinition> InstalledApps { get; }
        event EventHandler AppsChanged;
        Task InitializeAsync();
        Task<WebAppDefinition> InstallAsync(string name, string startUri);
        void SetPinned(WebAppDefinition app, bool isPinned);
        void Uninstall(WebAppDefinition app);
        Task SaveFaviconAsync(WebAppDefinition app, string faviconUri);
    }
}
