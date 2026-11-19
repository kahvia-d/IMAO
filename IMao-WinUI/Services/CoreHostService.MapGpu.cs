using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

public sealed partial class CoreHostService
{
    // -1 follows the player's persisted settings. An observer override lasts one pipe connection.
    private volatile int trialMapGpuMask = -1;
    private readonly CancellationTokenSource observerStop = new();
    private Task? observerTask;
    public (bool Minimap, bool Viewport) EffectiveMapGpuSettings
    {
        get
        {
            int mask = trialMapGpuMask;
            return mask < 0 ? DesiredMapGpuSettings : ((mask & 1) != 0, (mask & 2) != 0);
        }
    }

    private int PreferredMapGpuMask()
    {
        var (minimap, viewport) = DesiredMapGpuSettings;
        return (minimap ? 1 : 0) | (viewport ? 2 : 0);
    }

    public async Task<bool> SaveMapGpuAsync(bool? minimap, bool? viewport)
    {
        await lifecycleLock.WaitAsync();
        try
        {
            var session = await EnsureStartedLockedAsync(default) ?? throw new IOException("核心尚未连接");
            var old = EffectiveMapGpuSettings;
            // During an experiment, save only the switch the player touched. Release the override
            // so the other switch resumes its own durable preference.
            var desired = DesiredMapGpuSettings;
            var mask = ((minimap ?? desired.Minimap) ? 1 : 0) | ((viewport ?? desired.Viewport) ? 2 : 0);
            await SendDataCommandLockedAsync(session, "mapGpu", new { mask }, default);
            try
            {
                configuration.Update(value => value with {
                    MinimapGpuAcceleration = minimap ?? value.MinimapGpuAcceleration,
                    ViewportGpuAcceleration = viewport ?? value.ViewportGpuAcceleration
                });
            }
            catch
            {
                await SendDataCommandLockedAsync(session, "mapGpu", new { mask = (old.Minimap ? 1 : 0) | (old.Viewport ? 2 : 0) }, default);
                throw;
            }
            trialMapGpuMask = -1;
            OnUi(() => OnPropertyChanged(nameof(Configuration)));
            return true;
        }
        catch (Exception e) { ReportUserError("地图 GPU 设置未能应用：" + e.Message); return false; }
        finally { lifecycleLock.Release(); }
    }

    internal async Task<JsonElement> ObserveMapGpuAsync(char command, CancellationToken token = default)
    {
        await lifecycleLock.WaitAsync(token);
        try
        {
            if (command != '?' && command != 'R' && command is not (>= '0' and <= '3'))
                throw new ArgumentException("未知地图观测指令");
            var session = active;
            if (command == 'R') trialMapGpuMask = -1; // Also restore the next startup after a lost core.
            if (session is null || !session.Pipe.IsConnected) throw new IOException("核心未连接");
            object args = command == '?' ? new { } : new { mask = command == 'R' ? PreferredMapGpuMask() : command - '0' };
            var state = await SendDataCommandLockedAsync(session, "mapGpu", args, token);
            if (command != '?')
            {
                trialMapGpuMask = command == 'R' ? -1 : state.GetProperty("mask").GetInt32();
                OnUi(() => OnPropertyChanged(nameof(Configuration)));
            }
            return state;
        }
        finally { lifecycleLock.Release(); }
    }

    internal void StartMapGpuObserver() => observerTask ??= Task.Run(() => ListenForMapGpuTrialsAsync(observerStop.Token));

    private async Task ListenForMapGpuTrialsAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            bool trial = false;
            try
            {
                using var pipe = new NamedPipeServerStream($"IMao.MapGpu.{Environment.ProcessId}", PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await pipe.WaitForConnectionAsync(token);
                var byteCommand = new byte[1];
                while (await pipe.ReadAsync(byteCommand, token) != 0)
                {
                    char command = (char)byteCommand[0];
                    trial |= command is >= '0' and <= '3';
                    string response;
                    try { response = (await ObserveMapGpuAsync(command, token)).GetRawText(); }
                    catch (Exception e) when (!token.IsCancellationRequested) { response = JsonSerializer.Serialize(new { error = e.Message }); }
                    await pipe.WriteAsync(Encoding.UTF8.GetBytes(response + "\n"), token);
                }
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { return; }
            catch (IOException e)
            {
                System.Diagnostics.Trace.TraceWarning("地图观测连接中断：" + e.Message);
                if (!token.IsCancellationRequested) await Task.Delay(250);
            }
            finally
            {
                if (trial && !token.IsCancellationRequested)
                {
                    try { await ObserveMapGpuAsync('R', token); }
                    catch (Exception e) { ReportUserError("恢复个人地图 GPU 设置失败：" + e.Message); }
                }
            }
        }
    }
}
