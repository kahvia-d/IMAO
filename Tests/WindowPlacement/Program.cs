using IMao_WinUI.Helpers;
var root = Path.Combine(Path.GetTempPath(), "placement-tests-" + Guid.NewGuid().ToString("N"));
var oldCwd = Environment.CurrentDirectory;
Directory.CreateDirectory(Path.Combine(root, "exe"));
Environment.CurrentDirectory = Path.Combine(root, "exe");
int checks = 0;
void Check(bool ok, string name) { if (!ok) throw new Exception(name); checks++; Console.WriteLine("PASS " + name); }
try {
var errors = new List<string>();
var store = new MainWindowPlacementStore(Path.Combine(root, "user"), errors.Add);
var normal = new PlacementBounds(-1800, 80, 900, 650);
var state = new SavedMainWindowPlacement(normal, 96);
Check(store.Load() is null && errors.Count == 0, "missing file is clean first launch");
store.Save(state);
Check(new MainWindowPlacementStore(Path.Combine(root,"user"), errors.Add).Load() == state, "fresh store restores last closed bounds");
Check(!Directory.EnumerateFileSystemEntries(Environment.CurrentDirectory).Any(), "executable directory remains untouched");
Check(Directory.GetFiles(Path.Combine(root,"user")).Length == 1, "atomic write leaves only placement file");
var work = new PlacementBounds(-1920,0,1920,1040);
Check(MainWindowPlacementStore.Restore(state,work,144) == new PlacementBounds(-1800,65,1350,975), "DPI change retains logical size and clamps bottom");
Check(MainWindowPlacementStore.Restore(state,new(0,0,1280,720),96) == new PlacementBounds(0,70,900,650), "removed negative monitor fits current work area");
Check(MainWindowPlacementStore.Restore(null,new(-1920,0,1920,1040),96) == new PlacementBounds(-1520,130,1120,780), "first startup centered on negative monitor");
var tracker = new MainWindowPlacementTracker();
tracker.Observe(normal,96,false);
tracker.Observe(new(0,0,1920,1040),96,true);
tracker.Observe(new(-32000,-32000,160,28),96,true);
Check(tracker.LastNormal == state,"maximized and minimized do not replace restored bounds");
File.WriteAllText(Path.Combine(root,"user","main-window-placement.json"),"{broken");
Check(store.Load() is null && errors.Count == 1,"malformed JSON diagnosed and ignored");
File.WriteAllText(Path.Combine(root,"user","main-window-placement.json"),"{\"Bounds\":{\"X\":0,\"Y\":0,\"Width\":-1,\"Height\":650},\"Dpi\":96}");
Check(store.Load() is null && errors.Count == 2,"invalid geometry diagnosed and ignored");
store.Save(new(new(0,0,double.NaN,650),96));
Check(errors.Count == 3,"nonfinite geometry rejected with diagnosis");
var blocked=Path.Combine(root,"blocked");File.WriteAllText(blocked,"file");
new MainWindowPlacementStore(blocked,errors.Add).Save(state);
Check(errors.Count == 4,"write failure diagnosed without throwing");
Console.WriteLine($"ALL {checks} PLACEMENT ASSERTIONS PASSED");
} finally { Environment.CurrentDirectory=oldCwd; Directory.Delete(root,true); }
