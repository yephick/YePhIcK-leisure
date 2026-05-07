using System.Text;

namespace UartMonitor.Serial
{
    public sealed class LineEndingNormalizer
    {
        private char _pendingLineEnding = '\0';

        public string Normalize(string text)
        {
            if (string.IsNullOrEmpty(text))
                return text;

            StringBuilder output = new StringBuilder(text.Length + 1);
            int index = 0;

            if (_pendingLineEnding != '\0')
            {
                output.Append('\n');

                if (text[0] == '\r' || text[0] == '\n')
                {
                    index = 1;
                }
                _pendingLineEnding = '\0';
            }

            for (; index < text.Length; index++)
            {
                char ch = text[index];

                if (ch != '\r' && ch != '\n')
                {
                    output.Append(ch);
                    continue;
                }

                if (index + 1 >= text.Length)
                {
                    _pendingLineEnding = ch;
                    break;
                }

                char next = text[index + 1];
                if (next == '\r' || next == '\n')
                    index++;

                output.Append('\n');
            }

            return output.ToString();
        }
    }
}
