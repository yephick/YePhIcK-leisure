using Microsoft.VisualStudio.Imaging;
using Microsoft.VisualStudio.Shell;
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows;

namespace UartMonitor
{
    public class UartMonitorWindow : BaseToolWindow<UartMonitorWindow>
    {
        public override Type PaneType => typeof(Pane);

        public override string GetTitle(int toolWindowId)
        {
            return "UART Monitor";
        }

        public override async System.Threading.Tasks.Task<FrameworkElement> CreateAsync(int toolWindowId, CancellationToken cancellationToken)
        {
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            return new MyToolWindowControl();
        }

        [Guid("6a4f46a9-f6e5-4d36-a545-bba05f86eecf")]
        internal class Pane : ToolkitToolWindowPane
        {
            public Pane()
            {
                BitmapImageMoniker = KnownMonikers.Output;
            }
        }
    }
}
