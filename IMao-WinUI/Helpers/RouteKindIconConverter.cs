using IMao_WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media.Imaging;
using System.Runtime.InteropServices.WindowsRuntime;

namespace IMao_WinUI.Helpers;

/// <summary>
/// 路线列表里那个类型行的取值：有类型时给出每个类型自己的图标与名字，没有类型时给出占位文字。
/// </summary>
/// <remarks>
/// 和游戏内「路线列表」用的是同一批图标：核心在 <c>RouteKindSummary.IconPath</c> 里给出**绝对路径**
/// （它认识地图筛选目录里没有的类型，所以由核心解析，见 RoutePlanningState 的注释）。
///
/// 具体到 XAML 里用哪一档：
/// <list type="bullet">
/// <item><c>ConverterParameter=icon</c>：一个类型 → 它的图标（拿不到就是路径文字）。</item>
/// <item><c>ConverterParameter=text</c>：一个类型的名字（图标旁边那行字）。</item>
/// <item><c>ConverterParameter=empty</c>：整张类型表 → 空表时那句「自由点（无类型）」，否则空串。</item>
/// <item><c>ConverterParameter=hasIcon</c>：整张类型表 → 有没有可显示的图标，用来收掉占位行。</item>
/// </list>
///
/// 两件事必须照 <see cref="IMao_WinUI.Views.MapToolsWindow"/> 的做法来：
/// <list type="number">
/// <item>按**路径**缓存。同一个类型在几十行里反复出现，每行都读一次盘会把列表拖慢。</item>
/// <item>字节在这里读进来，**不交给图片自己去开文件**：这个程序以无包身份运行，图片加载器拿不到
/// 文件系统授权，而它失败时是静默的——行里会只剩文字，看不出为什么。读失败同样只让这一格退化成文字。</item>
/// </list>
/// 这个程序里没有 Svgg.Image 这类 SVG 解码器，所以只为 <c>.png</c> 解码；其余格式保留路径文字，
/// 宁可显示路径也不假装显示了一个图标。
/// </remarks>
public sealed class RouteKindIconConverter : IValueConverter
{
    private static readonly Dictionary<string, BitmapImage?> Cache = new(StringComparer.OrdinalIgnoreCase);

    public object Convert(object value, Type targetType, object parameter, string language) =>
        parameter as string switch
        {
            "icon" => value is string path && path.Length > 0 ? (object?)Load(path) ?? path : "",
            "text" => value is RouteKindSummary kind ? kind.Label : "",
            "empty" => Whole(value) is { Count: 0 } ? "自由点（无类型）" : "",
            "hasIcon" => HasIcon(value) ? Visibility.Visible : Visibility.Collapsed,
            _ => "",
        };

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();

    /// <summary>手绘路线只有自由点、没有类型，核心给出的类型表就是空的：那一行要说清是"自由点"而不是留白。</summary>
    private static IReadOnlyList<RouteKindSummary>? Whole(object value) =>
        value as IReadOnlyList<RouteKindSummary> ?? (value as RouteKindSummary[]);

    private static bool HasIcon(object value) =>
        Whole(value) is { Count: > 0 } kinds && kinds.Any(kind => kind.HasIcon);

    private static BitmapImage? Load(string path)
    {
        if (Cache.TryGetValue(path, out var cached)) return cached;
        BitmapImage? image = null;
        try
        {
            // Decoding is deferred, so a malformed file fails later, on the UI thread, where it can only
            // surface as an empty Image. Decode eagerly here instead: a broken icon then degrades to the
            // path text on this same call, which is visible in the row.
            var bytes = File.ReadAllBytes(path);
            using var stream = new MemoryStream(bytes);
            if (path.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
            {
                image = new BitmapImage();
                image.SetSource(stream.AsRandomAccessStream());
            }
            else image = null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException
            or NotSupportedException or FileNotFoundException or DirectoryNotFoundException)
        {
            image = null;
        }
        Cache[path] = image;
        return image;
    }
}

