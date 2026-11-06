using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

public partial class MainWindow : Window
{
    private BackendProcess? _backend;
    private readonly Dictionary<string, ChatWindow> _detached = new();

    public MainWindow()
    {
        InitializeComponent();
    }

    private static string WwwrootDir => Path.Combine(AppContext.BaseDirectory, "wwwroot");
    private static string BackendExe =>
        Environment.GetEnvironmentVariable("AI_COMPANION_BACKEND")
        ?? Path.Combine(AppContext.BaseDirectory, "backend", "backend.exe");
    private static string DataDir =>
        Environment.GetEnvironmentVariable("AI_COMPANION_DATA")
        ?? Path.Combine(AppContext.BaseDirectory, "data");

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        EnableRoundedCorners();
        try
        {
            _backend = new BackendProcess(BackendExe, DataDir, ReadApiPort());
            _backend.StatusChanged += OnBackendStatusChanged;
            await _backend.StartAsync();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this,
                "后端启动失败：" + ex.Message + "\n\n请确认 backend.exe 已构建并位于程序目录。",
                "Orua", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        await InitWebViewAsync();
    }

    internal static int ReadApiPort()
    {
        var cfgPath = Path.Combine(DataDir, "config.json");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(cfgPath));
            var srv = doc.RootElement.GetProperty("server");
            int port = srv.GetProperty("port").GetInt32();
            if (srv.TryGetProperty("actual_port", out var ap) && ap.GetInt32() > 0)
                return ap.GetInt32();
            return port;
        }
        catch
        {
            return 8765;
        }
    }

    private void OnBackendStatusChanged(string status, string detail)
    {
        if (status == "down")
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "down", detail } });
        else if (status == "up")
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "up", detail = "" } });
    }

    private void PostToJs(object msg)
    {
        try
        {
            Dispatcher.Invoke(() =>
                WebView.CoreWebView2?.PostWebMessageAsJson(JsonSerializer.Serialize(msg)));
        }
        catch { }
    }

    private async Task InitWebViewAsync()
    {
        var env = await CoreWebView2Environment.CreateAsync(
            null, Path.Combine(AppContext.BaseDirectory, "WebView2Data"));
        await WebView.EnsureCoreWebView2Async(env);
        WebView.CoreWebView2.SetVirtualHostNameToFolderMapping(
            "app.local", WwwrootDir, CoreWebView2HostResourceAccessKind.Allow);
        int port = _backend?.ApiPort ?? ReadApiPort();
        await WebView.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync(
            $"window.__API_BASE__ = 'http://127.0.0.1:{port}';");
        WebView.CoreWebView2.WebMessageReceived += OnWebMessageReceived;
        WebView.CoreWebView2.Navigate("https://app.local/index.html");
    }

    private void OnWebMessageReceived(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        try
        {
            using var doc = JsonDocument.Parse(e.WebMessageAsJson);
            if (!doc.RootElement.TryGetProperty("type", out var typeEl) ||
                typeEl.GetString() != BridgeProtocol.MsgWindow)
                return;
            var action = doc.RootElement.GetProperty("payload").GetProperty("action").GetString();
            Dispatcher.Invoke(() =>
            {
                switch (action)
                {
                    case BridgeProtocol.ActionMinimize: WindowState = WindowState.Minimized; break;
                    case BridgeProtocol.ActionMaximize: WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; break;
                    case BridgeProtocol.ActionClose: Close(); break;
                    case BridgeProtocol.ActionDrag: BeginCaptionDrag(); break;
                    case BridgeProtocol.ActionMaxDbl: SendCaptionDblClk(); break;
                    case BridgeProtocol.ActionSysMenu: ShowSystemMenu(); break;
                    case BridgeProtocol.ActionOpenDetach:
                        {
                            var target = doc.RootElement.GetProperty("payload")
                                .GetProperty("target").GetString();
                            if (string.IsNullOrEmpty(target)) break;
                            if (_detached.TryGetValue(target, out var existing)) {
                                existing.Activate();
                                break;
                            }
                            var w = new ChatWindow(target, _backend?.ApiPort ?? ReadApiPort());
                            _detached[target] = w;
                            w.Closed += (_, _) => _detached.Remove(target);
                            w.Show();
                            break;
                        }
                }
            });
        }
        catch { }
    }

    // 标题栏交互：v1 的 WM_NCHITTEST→HTCAPTION 方案失效——鼠标按下命中 WebView2
    // 子窗口，其客户区 WM_LBUTTONDOWN 直接投递，顶层窗口 HTCAPTION 判定不参与路由。
    // 现由 JS 转发事件，合成 WM_NCLBUTTONDOWN 走 DefWindowProc 原生拖拽循环。
    private const int WmNcLButtonDown = 0x00A1;
    private const int WmNcLButtonDblClk = 0x00A3;
    private const int WmSysCommand = 0x0112;
    private const int HtCaption = 2;
    private const uint TpmReturnCmd = 0x0100;
    private const uint TpmLeftButton = 0x0000;
    private const uint TpmRightButton = 0x0002;

    private void BeginCaptionDrag()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        _ = ReleaseCapture();
        _ = SendMessage(hwnd, WmNcLButtonDown, (IntPtr)HtCaption, IntPtr.Zero);
    }

    private void SendCaptionDblClk()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        _ = SendMessage(hwnd, WmNcLButtonDblClk, (IntPtr)HtCaption, IntPtr.Zero);
    }

    private void ShowSystemMenu()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        IntPtr menu = GetSystemMenu(hwnd, false);
        if (menu == IntPtr.Zero) return;
        GetCursorPos(out var pt);
        uint cmd = TrackPopupMenu(menu, TpmReturnCmd | TpmLeftButton | TpmRightButton,
                                  pt.X, pt.Y, 0, hwnd, IntPtr.Zero);
        if (cmd != 0) _ = SendMessage(hwnd, WmSysCommand, (IntPtr)cmd, IntPtr.Zero);
    }

    private void EnableRoundedCorners()
    {
        try
        {
            var hwnd = new WindowInteropHelper(this).Handle;
            const int dwmwaWindowCornerPreference = 33;
            const int dwmwcpRound = 2;
            int pref = dwmwcpRound;
            _ = DwmSetWindowAttribute(hwnd, dwmwaWindowCornerPreference, ref pref, sizeof(int));
        }
        catch { }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr GetSystemMenu(IntPtr hwnd, bool bRevert);

    [DllImport("user32.dll")]
    private static extern uint TrackPopupMenu(IntPtr hMenu, uint flags, int x, int y,
                                              int reserved, IntPtr hwnd, IntPtr rect);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point pt);

    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X, Y; }

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        foreach (var w in _detached.Values.ToList()) { try { w.Close(); } catch { } }
        _backend?.Dispose();
    }
}
