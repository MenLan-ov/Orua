using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

/// <summary>独立聊天窗口：复用同一前端（detached 模式），不持有后端守护。</summary>
public partial class ChatWindow : Window
{
    private readonly string _target;

    public ChatWindow(string target, int apiPort, string? title = null)
    {
        InitializeComponent();
        _target = target;
        // 标题优先用前端传来的显示名（Bot 名/群名，已在壳侧做无害化）；缺失时回退通用文案
        Title = string.IsNullOrWhiteSpace(title)
            ? (target.StartsWith("group:", StringComparison.Ordinal) ? "群聊" : "聊天")
            : title.Trim();
        _apiPort = apiPort;
    }

    private readonly int _apiPort;

    private static string WwwrootDir => Path.Combine(AppContext.BaseDirectory, "wwwroot");

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        try
        {
            await InitWebViewAsync();
        }
        catch (Exception ex)
        {
            Title = "聊天窗口初始化失败";
            ShowFailure(ex.Message);
        }
    }

    private void ShowFailure(string detail)
    {
        WebView.Visibility = Visibility.Collapsed;
        Root.Children.Add(new TextBlock
        {
            Text = "聊天窗口初始化失败：Web 视图无法启动。\n" + detail,
            Foreground = Brushes.White,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(28),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        });
    }

    internal void PostToWeb(string json)
    {
        try { WebView.CoreWebView2?.PostWebMessageAsJson(json); }
        catch { }
    }

    private async Task InitWebViewAsync()
    {
        var env = await CoreWebView2Environment.CreateAsync(
            null, Path.Combine(AppContext.BaseDirectory, "WebView2Data-detached"));
        await WebView.EnsureCoreWebView2Async(env);
        WebView.CoreWebView2.SetVirtualHostNameToFolderMapping(
            "app.local", WwwrootDir, CoreWebView2HostResourceAccessKind.Allow);
        await WebView.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync(
            $"window.__API_BASE__ = 'http://127.0.0.1:{_apiPort}';" +
            $"window.__DETACH_TARGET__ = {JsonSerializer.Serialize(_target)};" +
            $"window.__ORUA_TOKEN__ = {JsonSerializer.Serialize(MainWindow.ReadAuthToken())};");
        WebView.CoreWebView2.Navigate(
            "https://app.local/index.html?window=detached&target=" +
            System.Uri.EscapeDataString(_target));
    }
}
