namespace UartMonitor.Serial
{
    public static class AlignmentMath
    {
        public static double GetHorizontalScaleFactor(double visibleTextColumns, double hexColumns)
        {
            if (visibleTextColumns <= 0 || hexColumns <= 0)
                return 3.0;

            return hexColumns / visibleTextColumns;
        }
    }
}
