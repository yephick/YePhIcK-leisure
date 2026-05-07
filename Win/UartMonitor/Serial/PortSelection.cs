using System.Collections.Generic;
using System.Linq;

namespace UartMonitor.Serial
{
    public static class PortSelection
    {
        public static string? ChoosePortSelection(string? selected, string? savedSelection, IReadOnlyCollection<string> ports)
        {
            if (!string.IsNullOrWhiteSpace(selected) && ports.Contains(selected))
                return selected;

            if (!string.IsNullOrWhiteSpace(savedSelection) && ports.Contains(savedSelection))
                return savedSelection;

            return null;
        }
    }
}
