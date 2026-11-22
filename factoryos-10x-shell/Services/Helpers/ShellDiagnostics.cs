using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
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
        private const string DisableLoggingKey = "CoreShell.DisableAllLogging";
        private static int s_loggingDisabled;
        private static IntPtr s_savedStdOut, s_savedStdError;

        [DllImport("api-ms-win-core-processenvironment-l1-1-0.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SetEnvironmentVariableW(string name, string value);
        [DllImport("api-ms-win-core-processenvironment-l1-1-0.dll")]
        private static extern IntPtr GetStdHandle(int handle);
        [DllImport("api-ms-win-core-processenvironment-l1-1-0.dll", SetLastError = true)]
        private static extern bool SetStdHandle(int handle, IntPtr value);

        internal static bool IsLoggingEnabled => Volatile.Read(ref s_loggingDisabled) == 0;

        internal static bool DisableAllLogging
        {
            get => !IsLoggingEnabled;
            set
            {
                ApplicationData.Current.LocalSettings.Values[DisableLoggingKey] = value;
                ApplyLoggingPreference(value);
            }
        }

        private static void ApplyLoggingPreference(bool disabled)
        {
            // Native hosts and xul read the process environment directly.
            if (!SetEnvironmentVariableW("DESKTOPMODE_DISABLE_LOGGING", disabled ? "1" : null))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            bool wasDisabled = Interlocked.Exchange(ref s_loggingDisabled, disabled ? 1 : 0) != 0;
            if (disabled && !wasDisabled)
            {
                s_savedStdOut = GetStdHandle(-11);
                s_savedStdError = GetStdHandle(-12);
                SetStdHandle(-11, IntPtr.Zero);
                SetStdHandle(-12, IntPtr.Zero);
            }
            else if (!disabled && wasDisabled)
            {
                SetStdHandle(-11, s_savedStdOut);
                SetStdHandle(-12, s_savedStdError);
            }
        }

        [Conditional("DEBUG")]
        internal static void WriteDebug(string message)
        {
            if (IsLoggingEnabled) Debug.WriteLine(message);
        }

        private const long MaxLogBytes = 512 * 1024;
        private const long MaxHangLogBytes = 32 * 1024;
        private static readonly object WriteLock = new object();
        private static readonly object HangWriteLock = new object();
        private static string s_path;
        private static string s_hangPath;
        private static string s_windowState = "Desktop not initialized";
        private static string s_pendingCheckpoint;
        private static int s_writerRunning;

        internal static void Initialize()
        {
            try
            {
                bool disabled = ApplicationData.Current.LocalSettings.Values.TryGetValue(DisableLoggingKey, out object saved) &&
                    saved is bool value && value;
                ApplyLoggingPreference(disabled);
                string logDirectory = ApplicationData.Current.LocalFolder.Path;
                s_path = Path.Combine(logDirectory, "Shell-diagnostics.log");
                s_hangPath = Path.Combine(logDirectory, "Shell-ui-hang.log");
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

        internal static void RecordCheckpoint(string stage)
        {
            if (!IsLoggingEnabled) return;
            DateTimeOffset timestamp = DateTimeOffset.Now;
            _ = Task.Run(() => Append(s_path, WriteLock, MaxLogBytes, stage, timestamp));
        }

        internal static void RecordUiHang(long elapsedMilliseconds, string lastOperation)
        {
            Append(s_hangPath, HangWriteLock, MaxHangLogBytes,
                $"UI dispatcher not responding; elapsed={elapsedMilliseconds} ms; " +
                $"lastCompletedOperation={lastOperation}; {Volatile.Read(ref s_windowState)}");
        }

        internal static void RecordException(string source, Exception error)
        {
            if (!IsLoggingEnabled) return;
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

        private static void Append(string entry) => Append(s_path, WriteLock, MaxLogBytes, entry);

        private static void Append(string path, object writeLock, long maxBytes, string entry, DateTimeOffset? timestamp = null)
        {
            if (path == null || !IsLoggingEnabled) return;
            try
            {
                lock (writeLock)
                {
                    if (!IsLoggingEnabled) return;
                    var file = new FileInfo(path);
                    if (file.Exists && file.Length >= maxBytes)
                    {
                        string previous = path + ".previous";
                        File.Delete(previous);
                        File.Move(path, previous);
                    }
                    File.AppendAllText(path, $"{timestamp ?? DateTimeOffset.Now:O} {entry}{System.Environment.NewLine}");
                }
            }
            catch (Exception) { } // Keep the original error path intact.
        }
    }
}