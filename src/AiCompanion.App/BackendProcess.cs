using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text.Json;

namespace AiCompanion.App;

/// <summary>后端进程守护：拉起、健康检查、崩溃自动重启（≤3 次，1s/2s/4s 退避）、退出清理。</summary>
public sealed class BackendProcess : IDisposable
{
    public event Action<string, string>? StatusChanged;

    private readonly string _exePath;
    private readonly string _dataDir;
    private Process? _proc;
    private CancellationTokenSource? _cts;

    public BackendProcess(string exePath, string dataDir, int apiPort)
    {
        _exePath = exePath;
        _dataDir = dataDir;
        ApiPort = apiPort;
    }

    public int ApiPort { get; private set; }

    public async Task StartAsync(CancellationToken ct = default)
    {
        _cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var token = _cts.Token;
        if (!File.Exists(_exePath))
        {
            // 开发模式：backend.exe 未随程序输出，假定由外部（run_dev.cmd）启动
            await WaitHealthyAsync(TimeSpan.FromSeconds(15), token);
            return;
        }
        _ = Task.Run(() => RunLoopAsync(token));
        await WaitHealthyAsync(TimeSpan.FromSeconds(15), token);
    }

    private async Task RunLoopAsync(CancellationToken ct)
    {
        int restarts = 0;
        while (!ct.IsCancellationRequested)
        {
            StatusChanged?.Invoke("starting", "");
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = _exePath,
                    Arguments = $"--data \"{_dataDir}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                _proc = Process.Start(psi);
                if (_proc is null)
                {
                    StatusChanged?.Invoke("down", "无法启动后端进程");
                    return;
                }
                try
                {
                    // 重启后需等待就绪并广播 "up"，否则前端"连接中断"横幅不会消失
                    await WaitHealthyAsync(TimeSpan.FromSeconds(15), ct);
                }
                catch (TimeoutException)
                {
                    StatusChanged?.Invoke("down", "后端重启后未能就绪");
                    Kill();
                }
                await _proc.WaitForExitAsync(ct);
            }
            catch (OperationCanceledException)
            {
                Kill();
                return;
            }
            if (ct.IsCancellationRequested) return;

            if (restarts >= 3)
            {
                StatusChanged?.Invoke("down", "后端多次崩溃，请检查 logs 后手动重启程序");
                return;
            }
            restarts++;
            StatusChanged?.Invoke("down", $"后端异常退出，正在重启（{restarts}/3）…");
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(Math.Pow(2, restarts - 1)), ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    private async Task WaitHealthyAsync(TimeSpan timeout, CancellationToken ct)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < timeout && !ct.IsCancellationRequested)
        {
            int port = ReadActualPort();
            try
            {
                var resp = await http.GetAsync($"http://127.0.0.1:{port}/health", ct);
                if (resp.IsSuccessStatusCode)
                {
                    ApiPort = port;
                    StatusChanged?.Invoke("up", "");
                    return;
                }
            }
            catch
            {
                // 后端尚未就绪，继续轮询
            }
            await Task.Delay(300, ct);
        }
        throw new TimeoutException($"后端 {timeout.TotalSeconds}s 内未就绪");
    }

    // 端口可能因 8765 被占用而回退（8766-8775），以 config.json 中后端写回的 actual_port 为准
    private int ReadActualPort()
    {
        var cfgPath = Path.Combine(_dataDir, "config.json");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(cfgPath));
            var srv = doc.RootElement.GetProperty("server");
            if (srv.TryGetProperty("actual_port", out var ap) && ap.GetInt32() > 0)
                return ap.GetInt32();
            if (srv.TryGetProperty("port", out var p))
                return p.GetInt32();
        }
        catch
        {
            // config.json 尚未生成（后端首次启动前），用初始端口探测
        }
        return ApiPort;
    }

    private void Kill()
    {
        try { _proc?.Kill(entireProcessTree: true); } catch { }
        _proc = null;
    }

    public void Dispose()
    {
        try { _cts?.Cancel(); } catch { }
        Kill();
        _cts?.Dispose();
    }
}
