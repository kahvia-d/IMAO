namespace IMao_WinUI.Services;

/// <summary>Local setting keys shared by the sync page and the automatic sync.</summary>
internal static class KuroSyncSettings
{
    // The synchronization target is no longer a typed id: it is the ledger the map is
    // showing, with the Kuro account that ledger's binding names.
    public const string State = "kuroSyncStateId";
    public const string ShowSynced = "kuroSyncShowSynced";
    public const string ShowAllRegions = "kuroSyncShowAllRegions";
    public const string Automatic = "kuroSyncAutoSync";
}
