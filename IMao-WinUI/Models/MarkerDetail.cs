namespace IMao_WinUI.Models;

// Point IDs are opaque strings: the official 19-digit IDs exceed JavaScript's safe integer range.
public sealed record MarkerSelection
{
    public string Scene { get; init; } = string.Empty;
    public string ProfileId { get; init; } = "local";
    public string NameId { get; init; } = string.Empty;
    public string PointId { get; init; } = string.Empty;
    public int StateId { get; init; }
    public int CountryId { get; init; }
    public string FloorId { get; init; } = string.Empty;
    public string Level { get; init; } = string.Empty;
    public bool Completed { get; init; }
    public double ScreenX { get; init; }
    public double ScreenY { get; init; }
}

public sealed record MarkerDetail
{
    public string PointId { get; init; } = string.Empty;
    public int StateId { get; init; }
    public int CountryId { get; init; }
    public string TypeId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string Description { get; init; } = string.Empty;
    public string FloorId { get; init; } = string.Empty;
    public string Level { get; init; } = string.Empty;
    public string[] PictureUrls { get; init; } = [];
    public string? GuideUrl { get; init; }
    public string SourceUrl { get; init; } = string.Empty;
    public string LastUpdateTime { get; init; } = string.Empty;
}

public sealed record MarkerDetailResult(MarkerDetail Detail, string Status, bool FromCache = false);
