using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Windows.Globalization;
using Windows.System.Profile;
using factoryos_10x_shell.Controls;

namespace factoryos_10x_shell.Services.Store
{
    internal static class StoreCatalog
    {
        internal static readonly HttpClient Client = new HttpClient();
        internal static string Family => AnalyticsInfo.VersionInfo.DeviceFamily;
        private static string Market => new GeographicRegion().CodeTwoLetter;
        private static string Language => ApplicationLanguages.Languages.FirstOrDefault() ?? "en-US";
        internal static async Task<(StorePreviewProduct[] Items, int? NextPage)> CollectionAsync(string media, string ranking, int page)
        {
            string url = "https://apps.microsoft.com/api/Reco/GetComputedProductsList?filteredCategories=AllProducts" +
                "&listName=" + Uri.EscapeDataString(ranking) + "&mediaType=" + Uri.EscapeDataString(media) +
                "&noItems=24&pgNo=" + page.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                "&gl=" + Uri.EscapeDataString(Market) + "&hl=" + Uri.EscapeDataString(Language);
            using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)))
            {
                return ParseCollection(await GetAsync(url, timeout.Token), media, page);
            }
        }

        private static (StorePreviewProduct[] Items, int? NextPage) ParseCollection(JObject json, string media, int page)
        {
            var list = json["productsList"] as JArray;
            if (list == null) throw new InvalidOperationException("The Store returned an invalid collection.");
            // The public rankings can return every card with isBrowsable=false.
            // The inclusion criteria here are the requested collection and usable card data.
            var cards = list.OfType<JObject>()
                .Select(p => MakeCard(p, p, "title", "description", "imageType", "url", media))
                .Where(p => p != null).GroupBy(p => p.Id).Select(g => g.First()).ToArray();
            int? nextPage = (int?)json["nextPageNumber"];
            return (cards, (bool?)json["hasMorePages"] == true && nextPage > page ? nextPage : null);
        }

        internal static async Task<JObject[]> ProductsAsync(string ids, CancellationToken token)
        {
            var json = await GetAsync("https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" +
                Uri.EscapeDataString(ids) + "&market=" + Market + "&languages=" + Language + ",en-US", token);
            return json["Products"]?.OfType<JObject>().ToArray() ?? Array.Empty<JObject>();
        }

        internal static async Task<JObject> ProductForFamilyAsync(string family, CancellationToken token)
        {
            var json = await GetAsync("https://displaycatalog.mp.microsoft.com/v7.0/products/lookup?alternateId=PackageFamilyName&value=" +
                Uri.EscapeDataString(family) + "&market=" + Market + "&languages=" + Language + ",en-US", token);
            return json["Products"]?.OfType<JObject>()
                .FirstOrDefault(p => string.Equals((string)p.SelectToken("Properties.PackageFamilyName"), family, StringComparison.OrdinalIgnoreCase));
        }

        internal static StorePreviewProduct Card(JObject product)
        {
            var local = product["LocalizedProperties"]?.First as JObject;
            if (local == null) return null;
            return MakeCard(product, local, "ProductTitle", "Description", "ImagePurpose", "Uri",
                (string)product["ProductType"]);
        }

        internal static async Task<StorePreviewProduct[]> SearchAsync(string query, CancellationToken token)
        {
            var json = await GetAsync("https://storeedgefd.dsx.mp.microsoft.com/v9.0/search?query=" +
                Uri.EscapeDataString(query) + "&market=" + Market + "&locale=" + Language + "&deviceFamily=" + Family, token);
            return json["Payload"]?["SearchResults"]?.OfType<JObject>()
                .Select(p => MakeCard(p, p, "Title", "LongDescription", "ImageType", "Url", (string)p["ProductFamilyName"]))
                .Where(p => p != null).GroupBy(p => p.Id).Select(g => g.First()).ToArray() ?? Array.Empty<StorePreviewProduct>();
        }

        internal static async Task<string[]> SuggestionsAsync(string query, CancellationToken token)
        {
            var json = await GetAsync("https://displaycatalog.mp.microsoft.com/v7.0/productfamilies/autosuggest?market=" +
                Market + "&languages=" + Language + "&productFamilyNames=apps,games&platformDependencyName=" + Family +
                "&query=" + Uri.EscapeDataString(query), token);
            return json["Results"]?.SelectMany(group => group["Products"]?.Children() ?? Enumerable.Empty<JToken>())
                .Select(p => (string)p["Title"]).Where(title => !string.IsNullOrWhiteSpace(title))
                .Distinct(StringComparer.CurrentCultureIgnoreCase).Take(8).ToArray() ?? Array.Empty<string>();
        }

        private static StorePreviewProduct MakeCard(JObject product, JObject local, string title, string description,
            string purpose, string url, string type)
        {
            string id = (string)product.GetValue("ProductId", StringComparison.OrdinalIgnoreCase), name = (string)local[title];
            if (string.IsNullOrEmpty(id) || string.IsNullOrWhiteSpace(name)) return null;
            var images = local.GetValue("Images", StringComparison.OrdinalIgnoreCase)?.OfType<JObject>().ToArray() ?? Array.Empty<JObject>();
            Uri Image(params string[] kinds)
            {
                foreach (string kind in kinds)
                {
                    string address = (string)images.FirstOrDefault(i => string.Equals((string)i[purpose], kind, StringComparison.OrdinalIgnoreCase))?[url];
                    var uri = ImageAddress(address);
                    if (uri != null) return uri;
                }
                return null;
            }
            var tile = ImageAddress((string)local["iconUrl"]) ?? Image("Tile", "Logo", "BoxArt") ?? new Uri("ms-appx:///Assets/Store/Store.png");
            var cover = Image("SuperHeroArt", "Hero", "Screenshot", "Poster") ?? ImageAddress((string)local["posterArtUrl"]) ?? tile;
            return new StorePreviewProduct(id.ToUpperInvariant(), name,
                type?.StartsWith("Game", StringComparison.OrdinalIgnoreCase) == true ? "Games" : "Apps",
                (string)local[description] ?? "", tile, cover)
            {
                Publisher = (string)local.GetValue("PublisherName", StringComparison.OrdinalIgnoreCase),
                Price = (string)local.GetValue("DisplayPrice", StringComparison.OrdinalIgnoreCase) ?? string.Empty,
                Screenshots = images.Where(i => string.Equals((string)i[purpose], "Screenshot", StringComparison.OrdinalIgnoreCase))
                    .Select(i => ImageAddress((string)i[url])).Where(uri => uri != null).Distinct().Take(8).ToArray()
            };
        }

        private static Uri ImageAddress(string address)
        {
            if (address?.StartsWith("//") == true) address = "https:" + address;
            return Uri.TryCreate(address, UriKind.Absolute, out var uri) && uri.Scheme == "https" ? uri : null;
        }

        private static async Task<JObject> GetAsync(string url, CancellationToken token)
        {
            using (var response = await Client.GetAsync(url, token))
            {
                response.EnsureSuccessStatusCode();
                return JObject.Parse(await response.Content.ReadAsStringAsync());
            }
        }

        internal static JObject FindPackage(JObject product, string family = null)
        {
            ulong.TryParse(AnalyticsInfo.VersionInfo.DeviceFamilyVersion, out ulong os);
            var architecture = Windows.ApplicationModel.Package.Current.Id.Architecture.ToString();
            return product["DisplaySkuAvailabilities"]?.OfType<JObject>()
                .Where(s => string.Equals((string)s["Sku"]?["SkuType"], "full", StringComparison.OrdinalIgnoreCase))
                .Where(s => s["Availabilities"]?.Any(a => (decimal?)a.SelectToken("OrderManagementData.Price.ListPrice") == 0 &&
                    a["Actions"]?.Values<string>().Contains("Fulfill") == true) == true)
                .SelectMany(s => s.SelectToken("Sku.Properties.Packages")?.OfType<JObject>() ?? Enumerable.Empty<JObject>())
                .Where(p => family == null || string.Equals((string)p["PackageFamilyName"], family, StringComparison.OrdinalIgnoreCase))
                .Where(p => new[] { "appx", "appxbundle", "msix", "msixbundle" }.Contains((string)p["PackageFormat"], StringComparer.OrdinalIgnoreCase))
                .Where(p => p["Architectures"]?.Values<string>().Any(a => string.Equals(a, architecture, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(a, "neutral", StringComparison.OrdinalIgnoreCase)) == true)
                .Where(p => p["PlatformDependencies"]?.Any(d => ((string)d["PlatformName"] == Family ||
                    (string)d["PlatformName"] == "Windows.Universal") && ((ulong?)d["MinVersion"] ?? 0) <= os) == true)
                .Where(p => Guid.TryParse((string)p.SelectToken("FulfillmentData.WuCategoryId"), out _))
                .OrderByDescending(p => (ulong?)p["Version"] ?? 0).FirstOrDefault();
        }
    }
}
