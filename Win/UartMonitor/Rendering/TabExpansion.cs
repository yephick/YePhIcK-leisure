using System;
using System.Text;

namespace UartMonitor.Rendering
{
    public static class TabExpansion
    {
        public static string Expand(string text, int tabSize, ref int column)
        {
            if (string.IsNullOrEmpty(text))
                return string.Empty;

            StringBuilder builder = new StringBuilder(text.Length);
            int safeTabSize = GetAllowedTabSize(tabSize);
            foreach (char ch in text)
            {
                if (ch == '\t')
                {
                    int spaces = safeTabSize - (column % safeTabSize);
                    builder.Append(' ', spaces);
                    column += spaces;
                    continue;
                }

                builder.Append(ch);
                column++;
            }

            return builder.ToString();
        }

        public static int GetExpandedLength(string text, int tabSize)
        {
            int column = 0;
            Expand(text, tabSize, ref column);
            return column;
        }

        public static int ModelOffsetToExpandedOffset(string text, int modelOffset, int tabSize)
        {
            int safeOffset = Math.Max(0, Math.Min(modelOffset, text.Length));
            int column = 0;
            int safeTabSize = GetAllowedTabSize(tabSize);
            for (int index = 0; index < safeOffset; index++)
            {
                if (text[index] == '\t')
                    column += safeTabSize - (column % safeTabSize);
                else
                    column++;
            }

            return column;
        }

        public static int ExpandedOffsetToModelOffset(string text, int expandedOffset, int tabSize)
        {
            int target = Math.Max(0, expandedOffset);
            int column = 0;
            int safeTabSize = GetAllowedTabSize(tabSize);
            for (int index = 0; index < text.Length; index++)
            {
                int nextColumn = text[index] == '\t'
                    ? column + safeTabSize - (column % safeTabSize)
                    : column + 1;
                if (target < nextColumn)
                    return index;
                if (target == nextColumn)
                    return index + 1;
                column = nextColumn;
            }

            return text.Length;
        }

        public static int GetAllowedTabSize(int value)
        {
            return value == 2 || value == 3 || value == 4 || value == 6 || value == 8 ? value : 8;
        }
    }
}
