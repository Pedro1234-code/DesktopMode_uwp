using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using factoryos_10x_shell.Library.Models.Hardware;
using factoryos_10x_shell.Library.Services.Hardware;
using BatteryReport = factoryos_10x_shell.Library.Models.Hardware.BatteryReport;
using Windows.Devices.Power;
using Windows.System.Power;

namespace factoryos_10x_shell.Services.Hardware
{
    internal class BatteryService : IBatteryService
    {
        public bool IsBatteryInstalled
        {
            get
            {
                BatteryReport report = GetStatusReport();
                return report.PowerStatus != BatteryPowerStatus.NotInstalled;
            }
        }

        public event EventHandler BatteryStatusChanged;

        public BatteryService()
        {
            try
            {
                Battery.AggregateBattery.ReportUpdated += AggregateBattery_ReportUpdated;
            }
            catch (Exception)
            {
                // A battery sensor is optional on Xbox and desktop devices.
            }
        }

        public BatteryReport GetStatusReport()
        {
            Windows.Devices.Power.BatteryReport report;
            try
            {
                report = Battery.AggregateBattery.GetReport();
            }
            catch (Exception)
            {
                return new BatteryReport
                {
                    PowerStatus = BatteryPowerStatus.NotInstalled,
                    ChargePercentage = 0
                };
            }

            BatteryReport batteryReport = new BatteryReport()
            {
                ChargeRate = report.ChargeRateInMilliwatts.GetValueOrDefault(0),
                FullCapacity = report.FullChargeCapacityInMilliwattHours.GetValueOrDefault(0),
                RemainingCapacity = report.RemainingCapacityInMilliwattHours.GetValueOrDefault(0)
            };

            batteryReport.PowerStatus = report.Status switch
            {
                BatteryStatus.NotPresent => BatteryPowerStatus.NotInstalled,
                BatteryStatus.Idle => BatteryPowerStatus.Idle,
                BatteryStatus.Charging => BatteryPowerStatus.Charging,
                BatteryStatus.Discharging => BatteryPowerStatus.Draining,
                _ => BatteryPowerStatus.NotInstalled
            };

            // Some devices report Idle/Charging without a known full capacity.
            // A zero denominator here used to abort creation of the taskbar.
            if (batteryReport.PowerStatus != BatteryPowerStatus.NotInstalled &&
                batteryReport.FullCapacity > 0)
            {
                double percentage = 100.0 * batteryReport.RemainingCapacity / batteryReport.FullCapacity;
                batteryReport.ChargePercentage = (int)Math.Max(0, Math.Min(100, Math.Ceiling(percentage)));
            }
            else
            {
                batteryReport.ChargePercentage = 0;
            }

            return batteryReport;
        }

        private void AggregateBattery_ReportUpdated(Battery sender, object args)
        {
            BatteryStatusChanged?.Invoke(this, EventArgs.Empty);
        }
    }
}
