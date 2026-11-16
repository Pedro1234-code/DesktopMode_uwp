using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.Storage;
using Windows.System;
using Windows.System.Profile;

namespace factoryos_10x_shell.Services.Helpers
{
    internal static class ShellDiagnostics
    {
        private const long MaxLogBytes = 512 * 1024;
        private static readonly object WriteLock = new object();
        private static string s_path;
        private static string s_windowState = "Desktop not initialized";
        private static string s_pendingCheckpoint;
        private static int s_writerRunning;

        internal static void Initialize()
        {
            try
            {
                s_path = Path.Combine(ApplicationData.Current.LocalFolder.Path, "Shell-diagnostics.log");
                var version = Package.Current.Id.Version;
                Append($"Session started. Package={version.Major}.{version.Minor}.{version.Build}.{version.Revision}; " +
                    $"device={AnalyticsInfo.VersionInfo.DeviceFamily}; OS={AnalyticsInfo.VersionInfo.DeviceFamilyVersion}");
            }
            catch (Exception) { }
        }

        internal static void UpdateWindows(string state)
        {
            if (string.Equals(Interlocked.Exchange(ref s_windowState, state), state, StringComparison.Ordinal))
                return;
            Interlocked.Exchange(ref s_pendingCheckpoint, state);
            ScheduleCheckpoint();
        }

        private static void ScheduleCheckpoint()
        {
            if (Interlocked.CompareExchange(ref s_writerRunning, 1, 0) != 0) return;
            _ = Task.Run(async () =>
            {
                try
                {
                    await Task.Delay(200).ConfigureAwait(false);
                    string state = Interlocked.Exchange(ref s_pendingCheckpoint, null);
                    if (state != null) Append("Windows: " + state);
                }
                finally
                {
                    Interlocked.Exchange(ref s_writerRunning, 0);
                    if (Volatile.Read(ref s_pendingCheckpoint) != null) ScheduleCheckpoint();
                }
            });
        }

        internal static void RecordCheckpoint(string stage) => Append(stage);

        internal static void RecordException(string source, Exception error)
        {
            try
            {
                var details = new StringBuilder(source)
                    .AppendLine().Append("Windows: ").AppendLine(Volatile.Read(ref s_windowState));
                try
                {
                    details.Append("Memory: ").Append(MemoryManager.AppMemoryUsage)
                        .Append(" / ").Append(MemoryManager.AppMemoryUsageLimit).AppendLine(" bytes");
                }
                catch (Exception) { }

                // Messages and source paths can include private user data.
                int depth = 0;
                for (Exception current = error; current != null && depth++ < 8; current = current.InnerException)
                {
                    details.Append(current.GetType().FullName)
                        .Append(" (HRESULT 0x").Append(current.HResult.ToString("X8")).AppendLine(")");
                    details.AppendLine(new StackTrace(current, false).ToString());
                }
                Append(details.ToString().TrimEnd());
            }
            catch (Exception) { }
        }

        private static void Append(string entry)
        {
            if (s_path == null) return;
            try
            {
                lock (WriteLock)
                {
                    var file = new FileInfo(s_path);
                    if (file.Exists && file.Length >= MaxLogBytes)
                    {
                        string previous = s_path + ".previous";
                        File.Delete(previous);
                        File.Move(s_path, previous);
                    }
                    File.AppendAllText(s_path, $"{DateTimeOffset.Now:O} {entry}{System.Environment.NewLine}");
                }
            }
            catch (Exception) { } // Keep the original error path intact.
        }
    }
}