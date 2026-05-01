using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace UartMonitor.Serial
{
    public static class SelectionMirror
    {
        public static (int startLine, int endLine) GetSelectionLineRange(string text, int startOffset, int endOffset)
        {
            if (string.IsNullOrEmpty(text))
                return (0, 0);

            startOffset = Math.Max(0, Math.Min(startOffset, text.Length));
            endOffset = Math.Max(startOffset, Math.Min(endOffset, text.Length));

            int startLine = CountLinesBefore(text, startOffset);
            int endLine = CountLinesBefore(text, Math.Max(endOffset - 1, startOffset));
            return (startLine, endLine);
        }

        public static (int startOffset, int endOffset) GetLineRangeOffsets(string text, int startLine, int endLine)
        {
            List<(int start, int end)> lines = GetLineRanges(text);
            if (lines.Count == 0)
                return (0, 0);

            startLine = Math.Max(0, Math.Min(startLine, lines.Count - 1));
            endLine = Math.Max(startLine, Math.Min(endLine, lines.Count - 1));

            int startOffset = lines[startLine].start;
            int endOffset = lines[endLine].end;

            return (startOffset, endOffset);
        }

        public static (int startOffset, int endOffset) GetMirroredSelectionOffsets(
            string sourceText,
            int sourceStartOffset,
            int sourceEndOffset,
            string targetText)
        {
            return GetMirroredSelectionOffsets(sourceText, sourceStartOffset, sourceEndOffset, targetText, Encoding.GetEncoding(28591));
        }

        public static (int startOffset, int endOffset) GetMirroredSelectionOffsets(
            string sourceText,
            int sourceStartOffset,
            int sourceEndOffset,
            string targetText,
            Encoding encoding)
        {
            if (string.IsNullOrEmpty(sourceText) || string.IsNullOrEmpty(targetText))
                return (0, 0);

            sourceStartOffset = Math.Max(0, Math.Min(sourceStartOffset, sourceText.Length));
            sourceEndOffset = Math.Max(sourceStartOffset, Math.Min(sourceEndOffset, sourceText.Length));

            (int sourceStartLine, int sourceEndLine) = GetSelectionLineRange(sourceText, sourceStartOffset, sourceEndOffset);

            List<(int start, int end)> sourceLines = GetLineRanges(sourceText);
            List<(int start, int end)> targetLines = GetLineRanges(targetText);
            if (targetLines.Count == 0)
                return (0, 0);

            int targetStartLine;
            int targetEndLine;
            targetStartLine = Math.Max(0, Math.Min(sourceStartLine, targetLines.Count - 1));
            targetEndLine = Math.Max(targetStartLine, Math.Min(sourceEndLine, targetLines.Count - 1));

            if (sourceStartLine == sourceEndLine &&
                TryMirrorSingleLineSelection(
                    sourceText,
                    sourceLines[sourceStartLine],
                    sourceStartOffset,
                    sourceEndOffset,
                    targetText,
                    targetLines[targetStartLine],
                    encoding,
                    out int contentStart,
                    out int contentEnd))
            {
                return (contentStart, contentEnd);
            }

            int sourceLineStart = sourceLines[sourceStartLine].start;
            int sourceLineEnd = sourceLines[sourceEndLine].end;
            int sourceLineLength = Math.Max(1, sourceLineEnd - sourceLineStart);

            int targetLineStart = targetLines[targetStartLine].start;
            int targetLineEnd = targetLines[targetEndLine].end;
            int targetLineLength = Math.Max(1, targetLineEnd - targetLineStart);

            int relativeStart = sourceStartOffset - sourceLineStart;
            int relativeEnd = sourceEndOffset - sourceLineStart;

            double startRatio = (double)Math.Max(0, relativeStart) / sourceLineLength;
            double endRatio = (double)Math.Max(0, relativeEnd) / sourceLineLength;

            int mirroredStart = targetLineStart + (int)Math.Round(startRatio * targetLineLength);
            int mirroredEnd = targetLineStart + (int)Math.Round(endRatio * targetLineLength);

            mirroredStart = Math.Max(targetLineStart, Math.Min(mirroredStart, targetLineEnd));
            mirroredEnd = Math.Max(mirroredStart, Math.Min(mirroredEnd, targetLineEnd));

            return (mirroredStart, mirroredEnd);
        }

        private static bool TryMirrorSingleLineSelection(
            string sourceText,
            (int start, int end) sourceLine,
            int sourceStartOffset,
            int sourceEndOffset,
            string targetText,
            (int start, int end) targetLine,
            Encoding encoding,
            out int startOffset,
            out int endOffset)
        {
            startOffset = targetLine.start;
            endOffset = targetLine.start;

            if (sourceEndOffset <= sourceStartOffset)
                return false;

            int sourceLineStart = sourceLine.start;
            string selectedText = sourceText.Substring(sourceStartOffset, sourceEndOffset - sourceStartOffset);
            int sourceRelativeStart = Math.Max(0, sourceStartOffset - sourceLineStart);
            int sourceRelativeEnd = Math.Max(sourceRelativeStart, sourceEndOffset - sourceLineStart);

            string lineText = targetText.Substring(targetLine.start, targetLine.end - targetLine.start);
            byte[] bytes = ParseHexLine(lineText);
            if (bytes.Length == 0)
                return false;

            (string visibleText, List<int> byteMap) = DecodeVisibleText(bytes, encoding);
            if (visibleText.Length == 0 || byteMap.Count == 0)
                return false;

            int startCharIndex = Math.Max(0, Math.Min(sourceRelativeStart, visibleText.Length - 1));
            int endCharIndex = Math.Max(startCharIndex, Math.Min(sourceRelativeEnd - 1, visibleText.Length - 1));

            int visibleMatchIndex = visibleText.IndexOf(selectedText, startCharIndex, StringComparison.Ordinal);
            if (visibleMatchIndex < 0)
                visibleMatchIndex = visibleText.IndexOf(selectedText, StringComparison.Ordinal);

            if (visibleMatchIndex < 0)
                return false;

            int selectedStartChar = visibleMatchIndex >= 0 ? visibleMatchIndex : startCharIndex;
            int selectedEndChar = Math.Min(selectedStartChar + Math.Max(1, selectedText.Length) - 1, byteMap.Count - 1);

            int startByteIndex = byteMap[Math.Max(0, Math.Min(selectedStartChar, byteMap.Count - 1))];
            int endByteIndex = byteMap[Math.Max(0, Math.Min(selectedEndChar, byteMap.Count - 1))];

            startOffset = targetLine.start + (startByteIndex * 3);
            endOffset = targetLine.start + (endByteIndex * 3) + 2;
            endOffset = Math.Min(endOffset, targetLine.end);
            return true;
        }

        private static byte[] ParseHexLine(string lineText)
        {
            string[] tokens = lineText.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            return tokens
                .Where(token => token.Length == 2 && byte.TryParse(token, System.Globalization.NumberStyles.HexNumber, null, out _))
                .Select(token => byte.Parse(token, System.Globalization.NumberStyles.HexNumber))
                .ToArray();
        }

        private static (string visibleText, List<int> byteMap) DecodeVisibleText(byte[] bytes, Encoding encoding)
        {
            var builder = new StringBuilder();
            var byteMap = new List<int>();

            for (int index = 0; index < bytes.Length; index++)
            {
                byte current = bytes[index];

                if (current == 0x1B)
                {
                    if (index + 1 < bytes.Length && bytes[index + 1] == (byte)'[')
                    {
                        index += 1;
                        while (index + 1 < bytes.Length)
                        {
                            index++;
                            byte token = bytes[index];
                            if (token >= 0x40 && token <= 0x7E)
                                break;
                        }
                        continue;
                    }

                    if (index + 1 < bytes.Length && bytes[index + 1] == (byte)'c')
                    {
                        index++;
                    }

                    continue;
                }

                if (current == 0x0D || current == 0x0A)
                    continue;

                string text = encoding.GetString(new[] { current });
                if (string.IsNullOrEmpty(text))
                    continue;

                builder.Append(text);
                byteMap.Add(index);
            }

            return (builder.ToString(), byteMap);
        }

        private static int CountLinesBefore(string text, int charOffset)
        {
            int lines = 0;
            int limit = Math.Min(charOffset, text.Length);

            for (int index = 0; index < limit; index++)
            {
                if (text[index] == '\r' || text[index] == '\n')
                {
                    lines++;
                    if (index + 1 < limit && IsLineBreakPair(text[index], text[index + 1]))
                        index++;
                }
            }

            return lines;
        }

        private static List<(int start, int end)> GetLineRanges(string text)
        {
            var lines = new List<(int start, int end)>();
            int start = 0;

            for (int index = 0; index < text.Length; index++)
            {
                if (text[index] != '\r' && text[index] != '\n')
                    continue;

                int end = index + 1;
                if (index + 1 < text.Length && IsLineBreakPair(text[index], text[index + 1]))
                    end++;

                lines.Add((start, end));

                if (end > index + 1)
                    index++;

                start = end;
            }

            if (start <= text.Length)
                lines.Add((start, text.Length));

            return lines;
        }

        private static bool IsLineBreakPair(char first, char second)
        {
            return (first == '\r' && second == '\n') || (first == '\n' && second == '\r');
        }
    }
}
