namespace AiCompanion.App;

/// <summary>WebView2 双向消息协议常量（新增交互需在此登记）。</summary>
public static class BridgeProtocol
{
    public const string MsgWindow = "window";              // JS → WPF：窗口操作
    public const string MsgBackendStatus = "backend-status"; // WPF → JS：后端状态
    public const string MsgAuthToken = "auth-token";        // WPF → JS：本地 API 令牌（每会话启动/后端重启刷新）

    // window 消息的 action 取值
    public const string ActionMinimize = "minimize";
    public const string ActionMaximize = "maximize";
    public const string ActionClose = "close";
    public const string ActionDrag = "drag";        // 标题栏拖动（合成 WM_NCLBUTTONDOWN）
    public const string ActionMaxDbl = "maxdbl";    // 双击最大化/还原（合成 WM_NCLBUTTONDBLCLK）
    public const string ActionSysMenu = "sysmenu";  // 右键系统菜单（GetSystemMenu+TrackPopupMenu）
    public const string ActionOpenDetach = "opendetach";   // JS → WPF：打开独立聊天窗口
}
