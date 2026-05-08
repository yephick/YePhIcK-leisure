using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Windows.Media;

namespace UartMonitor.Rendering
{
    public sealed class AnsiParser
    {
        private readonly AnsiStyle _style = new AnsiStyle();
        private string _pending = string.Empty;
        private char _pendingLineEnding = '\0';

        public IReadOnlyList<LogSegment> Parse(string text)
        {
            string input = _pendingLineEnding == '\0' ? _pending + text : _pendingLineEnding + _pending + text;
            _pending = string.Empty;
            _pendingLineEnding = '\0';

            List<LogSegment> segments = new List<LogSegment>();
            StringBuilder buffer = new StringBuilder();
            StringBuilder prefixBuffer = new StringBuilder();
            bool sawEscape = false;

            for (int index = 0; index < input.Length; index++)
            {
                char ch = input[index];

                if (ch != '\x1b')
                {
                    StringBuilder target = sawEscape ? buffer : prefixBuffer;

                    if (ch == '\r' || ch == '\n')
                    {
                        if (index + 1 >= input.Length)
                        {
                            _pendingLineEnding = ch;
                            break;
                        }

                        char next = input[index + 1];
                        if ((next == '\r' || next == '\n') && next != ch)
                        {
                            index++;
                        }

                        target.Append('\n');
                        continue;
                    }

                    target.Append(ch);
                    continue;
                }

                if (index + 1 >= input.Length)
                {
                    _pending = input.Substring(index);
                    break;
                }

                if (input[index + 1] == 'c')
                {
                    _style.Reset();
                    sawEscape = true;
                    prefixBuffer.Clear();
                    segments.Add(new LogSegment(AnsiSequenceInfo.ToVisibleMarker(AnsiSequenceInfo.ResetTerminal), _style.Clone()));
                    index++;
                    continue;
                }

                if (input[index + 1] != '[')
                {
                    continue;
                }

                int end = -1;
                for (int search = index + 2; search < input.Length; search++)
                {
                    char final = input[search];
                    if ((final >= 'A' && final <= 'Z') || (final >= '[' && final <= '`') || (final >= 'a' && final <= '~'))
                    {
                        end = search;
                        break;
                    }
                }

                if (end < 0)
                {
                    _pending = input.Substring(index);
                    break;
                }

                if (prefixBuffer.Length > 0)
                {
                    buffer.Append(prefixBuffer);
                    prefixBuffer.Clear();
                }

                FlushText(segments, buffer);

                sawEscape = true;

                if (input[end] == 'm')
                    ApplySgr(input.Substring(index + 2, end - index - 2));
                else if (AnsiSequenceInfo.TryGetControlLabel(input.Substring(index + 2, end - index - 2), input[end], out string label))
                    segments.Add(new LogSegment(AnsiSequenceInfo.ToVisibleMarker(label), _style.Clone()));

                index = end;
            }

            if (!sawEscape && prefixBuffer.Length > 0)
                buffer.Append(prefixBuffer);

            FlushText(segments, buffer);
            return segments;
        }

        public void Reset()
        {
            _pending = string.Empty;
            _pendingLineEnding = '\0';
            _style.Reset();
        }

        private void FlushText(ICollection<LogSegment> segments, StringBuilder buffer)
        {
            if (buffer.Length == 0)
                return;

            segments.Add(new LogSegment(buffer.ToString(), _style.Clone()));
            buffer.Clear();
        }

        private void ApplySgr(string parameterText)
        {
            int[] codes = ParseCodes(parameterText);

            for (int index = 0; index < codes.Length; index++)
            {
                int code = codes[index];

                if (code == 0)
                    _style.Reset();
                else if (code == 1)
                    _style.Bold = true;
                else if (code == 4)
                    _style.Underline = true;
                else if (code == 5)
                    _style.Blink = true;
                else if (code == 9)
                    _style.Strikeout = true;
                else if (code == 22)
                    _style.Bold = false;
                else if (code == 24)
                    _style.Underline = false;
                else if (code == 25)
                    _style.Blink = false;
                else if (code == 29)
                    _style.Strikeout = false;
                else if (code == 39)
                    _style.Foreground = null;
                else if (code == 49)
                    _style.Background = null;
                else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97))
                    _style.Foreground = GetColor(code, false);
                else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107))
                    _style.Background = GetColor(code, true);
                else if ((code == 38 || code == 48) && index + 2 < codes.Length && codes[index + 1] == 5)
                {
                    Color color = GetXterm256Color(codes[index + 2]);

                    if (code == 38)
                        _style.Foreground = color;
                    else
                        _style.Background = color;

                    index += 2;
                }
            }
        }

        private static int[] ParseCodes(string parameterText)
        {
            if (string.IsNullOrEmpty(parameterText))
                return new[] { 0 };

            string[] parts = parameterText.Split(';');
            int[] codes = new int[parts.Length];

            for (int index = 0; index < parts.Length; index++)
            {
                if (!int.TryParse(parts[index], NumberStyles.None, CultureInfo.InvariantCulture, out codes[index]))
                    codes[index] = 0;
            }

            return codes;
        }

        private static Color GetColor(int code, bool background)
        {
            if (background)
                code -= code >= 100 ? 10 : 10;

            switch (code)
            {
                case 30: return Color.FromRgb(0x00, 0x00, 0x00);
                case 31: return Color.FromRgb(0xff, 0x00, 0x00);
                case 32: return Color.FromRgb(0x00, 0xff, 0x00);
                case 33: return Color.FromRgb(0xff, 0xff, 0x00);
                case 34: return Color.FromRgb(0x00, 0x80, 0xff);
                case 35: return Color.FromRgb(0xff, 0x00, 0xff);
                case 36: return Color.FromRgb(0x00, 0xff, 0xff);
                case 37: return Color.FromRgb(0xe5, 0xe5, 0xe5);
                case 90: return Color.FromRgb(0x80, 0x80, 0x80);
                case 91: return Color.FromRgb(0xff, 0x55, 0x55);
                case 92: return Color.FromRgb(0x55, 0xff, 0x55);
                case 93: return Color.FromRgb(0xff, 0xff, 0x55);
                case 94: return Color.FromRgb(0x55, 0x55, 0xff);
                case 95: return Color.FromRgb(0xff, 0x55, 0xff);
                case 96: return Color.FromRgb(0x55, 0xff, 0xff);
                case 97: return Color.FromRgb(0xff, 0xff, 0xff);
                default: return Color.FromRgb(0x00, 0xff, 0x00);
            }
        }

        private static Color GetXterm256Color(int code)
        {
            if (code < 0)
                code = 0;
            else if (code > 255)
                code = 255;

            if (code < 16)
                return GetBase16Color(code);

            if (code <= 231)
            {
                int value = code - 16;
                int red = value / 36;
                int green = value / 6 % 6;
                int blue = value % 6;

                return Color.FromRgb(ToColorCubeValue(red), ToColorCubeValue(green), ToColorCubeValue(blue));
            }

            byte level = (byte)(8 + ((code - 232) * 10));
            return Color.FromRgb(level, level, level);
        }

        private static Color GetBase16Color(int code)
        {
            switch (code)
            {
                case 0: return Color.FromRgb(0x00, 0x00, 0x00);
                case 1: return Color.FromRgb(0x80, 0x00, 0x00);
                case 2: return Color.FromRgb(0x00, 0x80, 0x00);
                case 3: return Color.FromRgb(0x80, 0x80, 0x00);
                case 4: return Color.FromRgb(0x00, 0x00, 0x80);
                case 5: return Color.FromRgb(0x80, 0x00, 0x80);
                case 6: return Color.FromRgb(0x00, 0x80, 0x80);
                case 7: return Color.FromRgb(0xc0, 0xc0, 0xc0);
                case 8: return Color.FromRgb(0x80, 0x80, 0x80);
                case 9: return Color.FromRgb(0xff, 0x00, 0x00);
                case 10: return Color.FromRgb(0x00, 0xff, 0x00);
                case 11: return Color.FromRgb(0xff, 0xff, 0x00);
                case 12: return Color.FromRgb(0x00, 0x00, 0xff);
                case 13: return Color.FromRgb(0xff, 0x00, 0xff);
                case 14: return Color.FromRgb(0x00, 0xff, 0xff);
                case 15: return Color.FromRgb(0xff, 0xff, 0xff);
                default: return Color.FromRgb(0x00, 0x00, 0x00);
            }
        }

        private static byte ToColorCubeValue(int value)
        {
            return (byte)(value == 0 ? 0 : 55 + (value * 40));
        }

    }
}
