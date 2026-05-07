using System;
using System.Text;

namespace UartMonitor.Rendering
{
    public static class AnsiSequenceInfo
    {
        public const string ResetTerminal = "ANSI reset terminal";
        public const string ClearScreen = "ANSI clear screen";
        public const string EraseDisplay = "ANSI erase display";
        public const string EraseLine = "ANSI erase line";
        public const string Ignored = "ANSI sequence ignored";

        public static string ToVisibleMarker(string label)
        {
            return $"<{label}>";
        }

        public static bool TryGetControlLabel(string parameters, char final, out string label)
        {
            if (final == 'J')
            {
                label = parameters == "2" ? ClearScreen : EraseDisplay;
                return true;
            }

            if (final == 'K')
            {
                label = EraseLine;
                return true;
            }

            label = string.Empty;
            return false;
        }

        public static bool TryGetLabel(byte[] bytes, out string label)
        {
            if (bytes.Length == 2 && bytes[0] == 0x1B && bytes[1] == (byte)'c')
            {
                label = ResetTerminal;
                return true;
            }

            if (bytes.Length >= 3 && bytes[0] == 0x1B && bytes[1] == (byte)'[')
            {
                char final = (char)bytes[bytes.Length - 1];
                string parameters = Encoding.ASCII.GetString(bytes, 2, bytes.Length - 3);
                return TryGetControlLabel(parameters, final, out label);
            }

            label = string.Empty;
            return false;
        }
    }
}
