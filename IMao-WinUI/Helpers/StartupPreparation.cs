namespace IMao_WinUI.Helpers;

internal static class StartupPreparation
{
    public static async Task<T> RunAsync<T>(Func<Task> firstFrame, Func<CancellationToken, Task<T>> prepare, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        await firstFrame().WaitAsync(cancellationToken);
        // Snapshot construction and the synchronous prefix of InitializeAsync perform disk work too.
        var result = await Task.Run(() => prepare(cancellationToken), cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        return result;
    }
}
