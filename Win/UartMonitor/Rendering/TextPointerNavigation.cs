using System;
using System.Windows.Documents;

namespace UartMonitor.Rendering
{
    public static class TextPointerNavigation
    {
        public static TextPointer GetAtTextOffset(TextPointer documentStart, int textOffset)
        {
            TextPointer current = documentStart;
            int remaining = Math.Max(0, textOffset);

            while (current != null)
            {
                if (current.GetPointerContext(LogicalDirection.Forward) == TextPointerContext.Text)
                {
                    string run = current.GetTextInRun(LogicalDirection.Forward);
                    if (remaining <= run.Length)
                        return current.GetPositionAtOffset(remaining, LogicalDirection.Forward) ?? current;

                    remaining -= run.Length;
                    current = current.GetPositionAtOffset(run.Length, LogicalDirection.Forward);
                    continue;
                }

                TextPointer next = current.GetNextContextPosition(LogicalDirection.Forward);
                if (next == null)
                    return current;

                string skippedText = new TextRange(current, next).Text;
                if (skippedText.Length > 0)
                {
                    if (remaining <= skippedText.Length)
                        return next;

                    remaining -= skippedText.Length;
                }

                current = next;
            }

            return documentStart.DocumentEnd;
        }
    }
}
