using System.Text;

namespace UartMonitor.Serial
{
    public static class HexChunkFormatter
    {
        public static string Format(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0)
                return string.Empty;

            StringBuilder output = new StringBuilder(bytes.Length * 4);
            StringBuilder line = new StringBuilder(bytes.Length * 3);

            for (int index = 0; index < bytes.Length; index++)
            {
                byte value = bytes[index];

                if (value == 0x0d || value == 0x0a)
                {
                    bool hasContent = line.Length > 0;

                    if (hasContent)
                    {
                        line.Append(' ');
                        line.Append(value.ToString("X2"));

                        if (index + 1 < bytes.Length)
                        {
                            byte next = bytes[index + 1];
                            if ((value == 0x0d && next == 0x0a) || (value == 0x0a && next == 0x0d))
                            {
                                line.Append(' ');
                                line.Append(next.ToString("X2"));
                                index++;
                            }
                        }
                    }

                    AppendLine(output, line);
                    continue;
                }

                if (line.Length > 0)
                    line.Append(' ');

                line.Append(value.ToString("X2"));
            }

            AppendLine(output, line);
            return output.ToString();
        }

        private static void AppendLine(StringBuilder output, StringBuilder line)
        {
            output.AppendLine(line.ToString());
            line.Clear();
        }
    }
}
