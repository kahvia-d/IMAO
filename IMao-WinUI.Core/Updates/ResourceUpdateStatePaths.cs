namespace IMao_WinUI.Core.Updates;

/// <summary>Development test trees never share resource activation or downloads with the player client.</summary>
public static class ResourceUpdateStatePaths
{
    public static string Resolve(string appRoot, string userRoot)
    {
        appRoot = Path.GetFullPath(appRoot);
        return File.Exists(Path.Combine(appRoot, "maptest-build-receipt.json"))
            ? Path.Combine(appRoot, ".maptest-state", "ResourceUpdates")
            : Path.Combine(userRoot, "ResourceUpdates");
    }
}
