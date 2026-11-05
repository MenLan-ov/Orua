namespace AiCompanion.App;

/// <summary>WebView2 双向消息协议常量（新增交互需在此登记）。</summary>
public static class BridgeProtocol
{
    public const string MsgWindow = "window";              // JS → WPF：窗口操作
    public const string MsgBackendStatus = "backend-status"; // WPF → JS：后端状态

    // window 消息的 action 取值
    public const string ActionMinimize = "minimize";
    public const string ActionMaximize = "maximize";
    public const string ActionClose = "close";
}
