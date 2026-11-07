using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

public partial class MainWindow : Window
{
    private const string DetachTargetPattern = @"\A(session|group):\d+\z";
    private const int DetachTitleMaxLength = 40;

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

    /// <summary>
    /// 数据目录固定到 %LOCALAPPDATA%\Orua，不再放在安装目录下的 data\。
    /// 原因：升级时 publish.cmd 会整目录重建 dist\Orua，原先 data\ 就在里面，
    /// 于是 app.db、上传的头像（uploads\*.png）、config.json 会被一并删掉——
    /// 用户表现为「更新后头像全没了」。数据与产物分离后，覆盖安装不再碰用户数据。
    /// </summary>
    private static string DataDir
    {
        get
        {
            var overridden = Environment.GetEnvironmentVariable("AI_COMPANION_DATA");
            if (!string.IsNullOrWhiteSpace(overridden)) return overridden;

            var roaming = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Orua");
            MigrateLegacyDataIfNeeded(roaming);
            return roaming;
        }
    }

    /// <summary>
    /// 一次性把旧版「安装目录\data」搬到新位置。只在目标尚不存在时执行，
    /// 因此重复启动、跨版本升级都不会覆盖新数据；搬迁失败不阻塞启动（退回旧目录）。
    /// </summary>
    private static void MigrateLegacyDataIfNeeded(string target)
    {
        try
        {
            var legacy = Path.Combine(AppContext.BaseDirectory, "data");
            if (!Directory.Exists(legacy)) return;
            if (Directory.Exists(target) && Directory.EnumerateFileSystemEntries(target).Any()) return;

            Directory.CreateDirectory(target);
            foreach (var dir in Directory.GetDirectories(legacy, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(target, Path.GetRelativePath(legacy, dir)));
            foreach (var file in Directory.GetFiles(legacy, "*", SearchOption.AllDirectories))
                File.Copy(file, Path.Combine(target, Path.GetRelativePath(legacy, file)), overwrite: false);

            // 旧库的 WAL/SHM 是运行期文件，直接拷贝可能不一致，让 SQLite 在新位置重建
            foreach (var sidecar in new[] { "app.db-wal", "app.db-shm" })
            {
                var p = Path.Combine(target, sidecar);
                if (File.Exists(p)) File.Delete(p);
            }

            System.Diagnostics.Debug.WriteLine($"[Orua] data migrated: {legacy} -> {target}");
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[Orua] data migration failed (fallback to install dir): {ex.Message}");
        }
    }

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

    internal static string ReadAuthToken()
    {
        var cfgPath = Path.Combine(DataDir, "config.json");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(cfgPath));
            var srv = doc.RootElement.GetProperty("server");
            if (srv.TryGetProperty("auth_token", out var tok) && tok.ValueKind == JsonValueKind.String)
                return tok.GetString() ?? "";
        }
        catch { }
        return "";
    }

    private void OnBackendStatusChanged(string status, string detail)
    {
        if (status == "down")
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "down", detail } });
        else if (status == "up")
        {
            // 后端每次重启都会重新生成令牌 → 重读 config.json 并推给前端
            var token = ReadAuthToken();
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "up", detail = "" } });
            PostToJs(new { type = BridgeProtocol.MsgAuthToken, payload = new { token } });
        }
    }

    private void PostToJs(object msg)
    {
        try
        {
            Dispatcher.Invoke(() =>
            {
                var json = JsonSerializer.Serialize(msg);
                WebView.CoreWebView2?.PostWebMessageAsJson(json);
                foreach (var w in _detached.Values.ToList()) w.PostToWeb(json);
            });
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
        string token = ReadAuthToken();
        await WebView.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync(
            $"window.__API_BASE__ = 'http://127.0.0.1:{port}'; window.__ORUA_TOKEN__ = {JsonSerializer.Serialize(token)};");
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
                            var payload = doc.RootElement.GetProperty("payload");
                            var target = payload.GetProperty("target").GetString();
                            if (string.IsNullOrEmpty(target)) break;
                            if (!Regex.IsMatch(target, DetachTargetPattern)) break;
                            string? rawName = null;
                            if (payload.TryGetProperty("name", out var nameEl) &&
                                nameEl.ValueKind == JsonValueKind.String)
                                rawName = nameEl.GetString();
                            var title = SanitizeDetachTitle(rawName);
                            if (_detached.TryGetValue(target, out var existing)) {
                                // T8 M1：最小化的旧窗口要先恢复，否则 Activate 看似调用成功但窗口仍不可见
                                if (existing.WindowState == WindowState.Minimized)
                                    existing.WindowState = WindowState.Normal;
                                existing.Activate();
                                break;
                            }
                            var w = new ChatWindow(target, _backend?.ApiPort ?? ReadApiPort(), title);
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

    /// <summary>独立窗口标题：剔除控制字符并截断到 40 字符；空名返回空串，由 ChatWindow 自行回退通用文案。</summary>
    private static string SanitizeDetachTitle(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return "";
        var sb = new System.Text.StringBuilder(name.Length);
        foreach (var ch in name)
            if (!char.IsControl(ch)) sb.Append(ch);
        var text = sb.ToString().Trim();
        return text.Length > DetachTitleMaxLength ? text[..DetachTitleMaxLength] : text;
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
