using System.Text;

namespace UartMonitor.Serial
{
    public static class HexChunkFormatter
    {
        public static string Format(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0)
                return string.Empty;

            StringBuilder line = new StringBuilder(bytes.Length * 3);

            for (int index = 0; index < bytes.Length; index++)
            {
                if (index > 0)
                    line.Append(' ');

                line.Append(bytes[index].ToString("X2"));
            }

            line.AppendLine();
            return line.ToString();
        }
    }
}
