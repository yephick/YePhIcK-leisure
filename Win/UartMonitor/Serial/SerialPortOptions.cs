using System.IO.Ports;
using System.Text;

namespace UartMonitor.Serial;

public sealed class SerialPortOptions
{
    public string PortName { get; set; } = "";
    public int BaudRate { get; set; } = 115200;
    public int DataBits { get; set; } = 8;
    public Parity Parity { get; set; } = Parity.None;
    public StopBits StopBits { get; set; } = StopBits.One;
    public Handshake Handshake { get; set; } = Handshake.None;
    public Encoding Encoding { get; set; } = Encoding.UTF8;
}
