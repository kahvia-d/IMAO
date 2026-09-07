// Only platform adapters are substituted. Tests compile the production services.
namespace Microsoft.UI.Dispatching
{
    public class DispatcherQueue
    {
        private static readonly DispatcherQueue instance = new();
        private readonly object gate = new();
        public static DispatcherQueue GetForCurrentThread() => instance;
        public bool HasThreadAccess => false;
        public bool TryEnqueue(Action action) { lock (gate) action(); return true; }
    }
}
namespace IMao_WinUI.Helpers { public static class RuntimeHelper { public static bool IsMSIX => false; } }
namespace Windows.ApplicationModel { }
namespace Windows.Storage
{
    public class ApplicationData
    {
        public static ApplicationData Current { get; } = new();
        public ApplicationDataContainer LocalSettings { get; } = new();
    }
    public class ApplicationDataContainer { public IDictionary<string, object> Values { get; } = new Dictionary<string, object>(); }
}
