namespace IMao_WinUI.Models;

/// <summary>
/// 手柄导航列表：一组可选项，以及"当前选中的是哪一个"。
///
/// 这个类存在的唯一理由是修一个实机 bug（地图工具台路径自动规划页"左摇杆只能上下、不能右推"）：
/// 窗口原先用指令键（按钮 Tag）当选中身份，但**同一个键可以出现在多个按钮上**——
/// 草稿为空但仍有活动路线时，「开始选点」和「新建路线」都发 new。用键找回选中项会永远命中
/// 第一个，于是右推左摇杆把选中项移到第二个按钮之后，紧接着的导航表重建又把它拉回第一个，
/// 界面上纹丝不动，看起来就是"右推没用"。
///
/// 因此身份必须是"哪一个控件"（引用身份，天然唯一）；键只在列表里唯一时才作为退路，
/// 用于控件被重建但功能没变的场景。
/// </summary>
internal sealed class GamepadNavigationList
{
    /// <summary>一个可选项。<paramref name="Control"/> 是它在界面上的身份，<paramref name="Key"/> 是它要执行的指令。</summary>
    internal readonly record struct Entry(object Control, string Key);

    private readonly List<Entry> items = [];
    private object? control;
    private string key = "";

    internal int Count => items.Count;
    internal int Index { get; private set; }
    internal Entry this[int index] => items[index];
    internal string CurrentKey => Count == 0 ? "" : items[Index].Key;
    internal object? CurrentControl => Count == 0 ? null : items[Index].Control;

    internal void Reset()
    {
        items.Clear(); Index = 0; control = null; key = "";
    }

    /// <summary>按最新的界面顺序重建，并尽量让"用户刚才选中的那个控件"保持不变。</summary>
    internal void Rebuild(IReadOnlyList<Entry> next)
    {
        items.Clear();
        items.AddRange(next);
        if (Count == 0) { Index = 0; control = null; key = ""; return; }
        int index = control is null ? -1 : items.FindIndex(item => ReferenceEquals(item.Control, control));
        // 控件被重建（按钮列表换了）时退回指令键，但只在这个键唯一时才可信：重键一律不认，
        // 否则就会重现上面那个"移过去又被拉回来"的死循环。
        if (index < 0 && key.Length > 0 && items.Count(item => item.Key == key) == 1)
            index = items.FindIndex(item => item.Key == key);
        Index = index >= 0 ? index : Math.Clamp(Index, 0, Count - 1);
        Adopt();
    }

    /// <summary>方向选择算出的目标下标（越界会被夹在列表范围内）。</summary>
    internal void Select(int index)
    {
        if (Count == 0) return;
        Index = Math.Clamp(index, 0, Count - 1);
        Adopt();
    }

    private void Adopt()
    {
        control = items[Index].Control;
        key = items[Index].Key;
    }
}
