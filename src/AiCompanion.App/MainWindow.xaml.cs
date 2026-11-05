using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

public partial class MainWindow : Window
{
    private BackendProcess? _backend;

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
        InstallCaptionHitTest();
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
                "AI 陪伴", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        await InitWebViewAsync();
    }

    private static int ReadApiPort()
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
                }
            });
        }
        catch { }
    }

    // 标题栏拖拽：WM_NCHITTEST 由系统直接发给顶层 WPF 窗口（即使光标
    // 位于 WebView2 子 HWND 上），对标题栏条带返回 HTCAPTION 即可获得
    // 系统原生拖拽/双击最大化/右键系统菜单，无需 JS 转发。
    private const int WmNcHitTest = 0x0084;
    private const int HtCaption = 2;
    private const double TitlebarDip = 42;   // 与 css 的 #titlebar 高度一致
    private const double ActionsDip = 130;   // 右侧 3 个按钮 + 间距 + 内边距

    private bool IsCaptionStrip(IntPtr win, int x, int y)
    {
        GetClientRect(win, out var rc);
        var pt = new Point();
        ClientToScreen(win, ref pt);
        double dpi = GetDpiForWindow(win) / 96.0;
        double top = pt.Y;
        double right = pt.X + rc.Right;
        return x >= pt.X && x < right - ActionsDip * dpi &&
               y >= top && y < top + TitlebarDip * dpi;
    }

    private void InstallCaptionHitTest()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        HwndSource.FromHwnd(hwnd)?.AddHook(WndProc);
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmNcHitTest)
        {
            var x = unchecked((short)(long)lParam);
            var y = unchecked((short)((long)lParam >> 16));
            if (IsCaptionStrip(hwnd, x, y))
            {
                handled = true;
                return (IntPtr)HtCaption;
            }
        }
        return IntPtr.Zero;
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
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hwnd, out Rect rc);

    [DllImport("user32.dll")]
    private static extern bool ClientToScreen(IntPtr hwnd, ref Point pt);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X, Y; }

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _backend?.Dispose();
    }
}
