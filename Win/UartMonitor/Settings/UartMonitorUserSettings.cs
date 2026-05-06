using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace UartMonitor.Settings
{
    public sealed class UartMonitorUserSettings
    {
        public static string? OverrideFilePathForTests;

        public string? PortName { get; set; }
        public string BaudRate { get; set; } = "115200";
        public string EncodingDisplayName { get; set; } = "ISO-8859-1: 1998 (Latin-1, West Europe)";
        public string FontFamily { get; set; } = "Consolas";
        public string FontSize { get; set; } = "16";
        public int TabSize { get; set; } = 8;
        public int DataBits { get; set; } = 8;
        public string StopBits { get; set; } = "1";
        public string Parity { get; set; } = "None";
        public string FlowControl { get; set; } = "XON/XOFF";
        public bool MergeLineEndings { get; set; } = true;
        public bool AutoScroll { get; set; } = true;
        public bool PanelSync { get; set; } = false;

        public static string FilePath =>
            OverrideFilePathForTests ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "UartMonitor", "settings.txt");

        public static UartMonitorUserSettings Load()
        {
            try
            {
                if (!File.Exists(FilePath))
                    return new UartMonitorUserSettings();

                Dictionary<string, string> values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (string line in File.ReadAllLines(FilePath))
                {
                    int equals = line.IndexOf('=');
                    if (equals <= 0)
                        continue;

                    string key = line.Substring(0, equals).Trim();
                    string value = line.Substring(equals + 1).Trim();
                    values[key] = value;
                }

                UartMonitorUserSettings settings = new UartMonitorUserSettings();
                settings.PortName = GetString(values, nameof(PortName));
                settings.BaudRate = GetString(values, nameof(BaudRate), settings.BaudRate);
                settings.EncodingDisplayName = GetString(values, nameof(EncodingDisplayName), settings.EncodingDisplayName);
                settings.FontFamily = GetString(values, nameof(FontFamily), settings.FontFamily);
                settings.FontSize = GetString(values, nameof(FontSize), settings.FontSize);
                settings.TabSize = GetAllowedTabSize(GetInt(values, nameof(TabSize), settings.TabSize));
                settings.DataBits = GetInt(values, nameof(DataBits), settings.DataBits);
                settings.StopBits = GetString(values, nameof(StopBits), settings.StopBits);
                settings.Parity = GetString(values, nameof(Parity), settings.Parity);
                settings.FlowControl = GetString(values, nameof(FlowControl), settings.FlowControl);
                settings.MergeLineEndings = GetBool(values, nameof(MergeLineEndings), settings.MergeLineEndings);
                settings.AutoScroll = GetBool(values, nameof(AutoScroll), settings.AutoScroll);
                settings.PanelSync = GetBool(values, nameof(PanelSync), settings.PanelSync);
                return settings;
            }
            catch
            {
                return new UartMonitorUserSettings();
            }
        }

        public void Save()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllLines(FilePath, new[]
            {
                $"PortName={PortName ?? string.Empty}",
                $"BaudRate={BaudRate}",
                $"EncodingDisplayName={EncodingDisplayName}",
                $"FontFamily={FontFamily}",
                $"FontSize={FontSize}",
                $"TabSize={GetAllowedTabSize(TabSize).ToString(CultureInfo.InvariantCulture)}",
                $"DataBits={DataBits.ToString(CultureInfo.InvariantCulture)}",
                $"StopBits={StopBits}",
                $"Parity={Parity}",
                $"FlowControl={FlowControl}",
                $"MergeLineEndings={MergeLineEndings}",
                $"AutoScroll={AutoScroll}",
                $"PanelSync={PanelSync}",
            });
        }

        private static string GetString(IReadOnlyDictionary<string, string> values, string key, string fallback = "")
        {
            return values.TryGetValue(key, out string? value) ? value : fallback;
        }

        private static int GetInt(IReadOnlyDictionary<string, string> values, string key, int fallback)
        {
            return values.TryGetValue(key, out string? value) && int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed)
                ? parsed
                : fallback;
        }

        private static bool GetBool(IReadOnlyDictionary<string, string> values, string key, bool fallback)
        {
            return values.TryGetValue(key, out string? value) && bool.TryParse(value, out bool parsed)
                ? parsed
                : fallback;
        }

        private static int GetAllowedTabSize(int value)
        {
            return value == 2 || value == 3 || value == 4 || value == 6 || value == 8
                ? value
                : 8;
        }
    }
}
