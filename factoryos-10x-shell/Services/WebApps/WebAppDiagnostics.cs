using factoryos_10x_shell.Library.Models.InternalData;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using Windows.Storage;

namespace factoryos_10x_shell.Services.WebApps
{
    // Callers supply only diagnostic fields, never URLs, titles or input text.
    internal static class WebAppDiagnostics
    {
        private static readonly object Gate = new object();
        private static readonly Queue<string> Pending = new Queue<string>();
        private static readonly Stopwatch Clock = Stopwatch.StartNew();
        private static readonly string Run = Guid.NewGuid().ToString("N").Substring(0, 8);
        private static bool draining;
        private static int dropped;

        internal static void Write(WebAppWindowModel window, string stage, string detail = "")
        {
            try
            {
                string backend = window?.App?.UseStrawfoxWindow == true ? "StrawfoxWindow" :
                    window?.App?.UseStrawfox == true ? "StrawfoxTab" : "WebView2";
                string id = window == null ? "none" : RuntimeHelpers.GetHashCode(window).ToString("X8");
                string line = $"{DateTimeOffset.UtcNow:O} run={Run} t={Clock.ElapsedMilliseconds}ms " +
                    $"thread={System.Environment.CurrentManagedThreadId} window={id} backend={backend} stage={stage} {detail}";
                Debug.WriteLine("[WebApps] " + line);
                lock (Gate)
                {
                    if (Pending.Count >= 1024) { dropped++; return; }
                    Pending.Enqueue(line);
                    if (draining) return;
                    draining = true;
                }
                _ = Task.Run(DrainAsync);
            }
            catch (Exception) { }
        }

        private static async Task DrainAsync()
        {
            while (true)
            {
                await Task.Delay(100).ConfigureAwait(false);
                string[] batch;
                int lost;
                lock (Gate)
                {
                    if (Pending.Count == 0) { draining = false; return; }
                    batch = Pending.ToArray();
                    Pending.Clear();
                    lost = dropped;
                    dropped = 0;
                }
                try
                {
                    string path = Path.Combine(ApplicationData.Current.LocalFolder.Path, "WebApps-runtime.log");
                    if (File.Exists(path) && new FileInfo(path).Length >= 2 * 1024 * 1024)
                    {
                        string previous = path + ".previous";
                        if (File.Exists(previous)) File.Delete(previous);
                        File.Move(path, previous);
                    }
                    string text = string.Join(System.Environment.NewLine, batch) + System.Environment.NewLine;
                    if (lost != 0) text += "WebApps diagnostics: dropped=" + lost + System.Environment.NewLine;
                    File.AppendAllText(path, text);
                }
                catch (Exception error) { Debug.WriteLine("[WebApps] log write failed: " + error.HResult.ToString("X8")); }
            }
        }
    }
}
