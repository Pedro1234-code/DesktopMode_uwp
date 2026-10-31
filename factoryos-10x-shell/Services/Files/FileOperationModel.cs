using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.UI.Xaml;

namespace factoryos_10x_shell.Services.Files
{
    internal enum FileOperationKind { Copy, Move, Delete }

    internal sealed class FileOperationProgress
    {
        public string Status { get; set; }
        public string Item { get; set; }
        public string Summary { get; set; }
        public double Percent { get; set; } = -1;
    }

    internal sealed class FileOperationResult
    {
        public List<IStorageItem> Succeeded { get; } = new List<IStorageItem>();
        public List<string> Errors { get; } = new List<string>();
        public bool Cancelled { get; set; }

        internal void AddError(string message)
        {
            if (Errors.Count < 50) Errors.Add(message);
            else if (Errors.Count == 50) Errors.Add("Additional errors were omitted.");
        }
    }

    internal sealed class FileOperationModel : INotifyPropertyChanged
    {
        private string m_status = "Preparing...";
        private string m_item = string.Empty;
        private string m_summary = string.Empty;
        private string m_errorDetails = string.Empty;
        private double m_percent;
        private bool m_indeterminate = true;
        private bool m_running = true;
        private bool m_cancelling;
        private Visibility m_visibility = Visibility.Visible;
        private TaskCompletionSource<string> m_password;

        internal FileOperationModel(string title, string location)
        {
            Title = title;
            Location = location;
        }

        public string Title { get; }
        public string Location { get; }
        public string Status { get => m_status; private set => Set(ref m_status, value); }
        public string Item { get => m_item; private set => Set(ref m_item, value); }
        public string Summary { get => m_summary; private set => Set(ref m_summary, value); }
        public string ErrorDetails { get => m_errorDetails; private set => Set(ref m_errorDetails, value); }
        public double Percent { get => m_percent; private set { Set(ref m_percent, value); Changed(nameof(ProgressText)); } }
        public bool IsIndeterminate { get => m_indeterminate; private set { Set(ref m_indeterminate, value); Changed(nameof(ProgressText)); } }
        public string ProgressText => IsIndeterminate ? string.Empty : Percent.ToString("0") + "%";
        public bool IsRunning => m_running;
        public bool CanCancel => !m_cancelling || !m_running;
        public string ActionText => m_running ? (m_cancelling ? "Cancelling..." : "Cancel") : "Close";
        public Visibility PasswordVisibility => m_password != null ? Visibility.Visible : Visibility.Collapsed;
        public Visibility ErrorVisibility => string.IsNullOrEmpty(ErrorDetails) ? Visibility.Collapsed : Visibility.Visible;
        public Visibility Visibility { get => m_visibility; internal set => Set(ref m_visibility, value); }
        internal CancellationTokenSource Cancellation { get; } = new CancellationTokenSource();
        internal Task<FileOperationResult> Completion { get; set; }
        internal bool CloseRequested { get; set; }
        internal List<string> AffectedFolderPaths { get; } = new List<string>();

        internal void Update(FileOperationProgress progress)
        {
            if (!m_running || m_cancelling || m_password != null) return;
            Status = progress.Status ?? Status;
            Item = progress.Item ?? string.Empty;
            Summary = progress.Summary ?? string.Empty;
            IsIndeterminate = progress.Percent < 0;
            if (!IsIndeterminate) Percent = Math.Max(0, Math.Min(99, progress.Percent));
        }

        internal void Cancel()
        {
            if (!m_running || m_cancelling) return;
            m_cancelling = true;
            Status = "Cancelling...";
            Cancellation.Cancel();
            Changed(nameof(CanCancel));
            Changed(nameof(ActionText));
        }

        internal async Task<string> RequestPasswordAsync()
        {
            Cancellation.Token.ThrowIfCancellationRequested();
            Status = "Password required";
            IsIndeterminate = false;
            m_password = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
            Changed(nameof(PasswordVisibility));
            try
            {
                using (Cancellation.Token.Register(() => m_password?.TrySetCanceled()))
                    return await m_password.Task;
            }
            finally
            {
                m_password = null;
                Changed(nameof(PasswordVisibility));
                Status = "Preparing...";
                IsIndeterminate = true;
            }
        }

        internal void SubmitPassword(string password)
        {
            if (!string.IsNullOrEmpty(password)) m_password?.TrySetResult(password);
        }

        internal void Finish(FileOperationResult result)
        {
            m_running = false;
            IsIndeterminate = false;
            Status = result.Cancelled ? "Cancelled" : result.Errors.Count > 0 ? "Finished with errors" : "Completed";
            if (!result.Cancelled && result.Errors.Count == 0) Percent = 100;
            ErrorDetails = string.Join(System.Environment.NewLine, result.Errors);
            Summary = result.Cancelled
                ? "Completed changes were kept. Partial folders or archives may remain."
                : result.Errors.Count > 0 ? "Some items could not be processed. See details below." : "All operations completed.";
            Changed(nameof(IsRunning));
            Changed(nameof(CanCancel));
            Changed(nameof(ActionText));
            Changed(nameof(ErrorVisibility));
        }

        public event PropertyChangedEventHandler PropertyChanged;
        private void Changed(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        private void Set<T>(ref T field, T value, [CallerMemberName] string name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return;
            field = value;
            Changed(name);
        }
    }
}
