using System;
using System.Timers;
using factoryos_10x_shell.Library.Services.Environment;

namespace factoryos_10x_shell.Services.Environment
{
    internal class TimeService : ITimeService, IDisposable
    {
        private readonly Timer updateCheck;
        private string lastClockText;

        public string ClockFormat { get; set; }
        public string DateFormat { get; set; }

        public TimeService()
        {
            ClockFormat = "h:mm";
            DateFormat = "M/d/yyyy";
            lastClockText = DateTime.Now.ToString(ClockFormat);

            // The clock only displays minutes. The previous 10 ms interval
            // woke a ThreadPool worker 100 times per second for the lifetime
            // of the Shell merely to check whether DateTime.Second changed.
            // Besides wasting CPU, those periodic wakeups showed up as small
            // pointer/composition hitches on Xbox.
            updateCheck = new Timer(1000)
            {
                AutoReset = true
            };
            updateCheck.Elapsed += UpdateCheck_Elapsed;

            updateCheck.Start();
        }

        private void UpdateCheck_Elapsed(object sender, ElapsedEventArgs e)
        {
            string clockText = DateTime.Now.ToString(ClockFormat);
            if (string.Equals(lastClockText, clockText, StringComparison.Ordinal)) return;
            lastClockText = clockText;
            UpdateClockBinding?.Invoke(this, EventArgs.Empty);
        }

        public event EventHandler UpdateClockBinding;

        public void Dispose()
        {
            updateCheck.Stop();
            updateCheck.Elapsed -= UpdateCheck_Elapsed;
            updateCheck.Dispose();
        }
    }
}
