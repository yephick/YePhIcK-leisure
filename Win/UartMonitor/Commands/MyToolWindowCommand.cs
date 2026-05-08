namespace UartMonitor
{
    [Command(PackageIds.MyCommand)]
    internal sealed class UartMonitorWindowCommand : BaseCommand<UartMonitorWindowCommand>
    {
        protected override Task ExecuteAsync(OleMenuCmdEventArgs e)
        {
            return UartMonitorWindow.ShowAsync();
        }
    }
}
