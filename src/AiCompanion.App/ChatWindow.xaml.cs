using System.IO;
using System.Text.Json;
using System.Windows;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

/// <summary>独立聊天窗口：复用同一前端（detached 模式），不持有后端守护。</summary>
public partial class ChatWindow : Window
{
    private readonly string _target;

    public ChatWindow(string target, int apiPort)
    {
        InitializeComponent();
        _target = target;
        Title = target.StartsWith("group:") ? "群聊" : "聊天";
        _apiPort = apiPort;
    }

    private readonly int _apiPort;

    private static string WwwrootDir => Path.Combine(AppContext.BaseDirectory, "wwwroot");

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        await InitWebViewAsync();
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
            $"window.__DETACH_TARGET__ = '{_target}';");
        WebView.CoreWebView2.Navigate(
            "https://app.local/index.html?window=detached&target=" +
            System.Uri.EscapeDataString(_target));
    }
}
