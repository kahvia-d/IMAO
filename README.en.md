<!-- markdownlint-disable MD033 MD041 -->

<div align="center">

<img alt="LOGO" src="Docs/images/readme/logo.png" width="148" height="148" />

# IMao · Wuthering Waves Map Tool

<br>

<div>
    <img alt="platform" src="https://img.shields.io/badge/platform-Windows%20x64-blueviolet">
    <img alt="license" src="https://img.shields.io/github/license/kahvia-d/IMAO">
</div>
<div>
    <img alt="GitHub release" src="https://img.shields.io/github/v/release/kahvia-d/IMAO?color=%2343e28a">
    <img alt="GitHub all releases" src="https://img.shields.io/github/downloads/kahvia-d/IMAO/total?style=social">
    <img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/kahvia-d/IMAO?style=social">
</div>
<br>

[简体中文](README.md) | [English](README.en.md)

**The KuroBBS interactive map, overlaid right on top of your game window.**

Markers, routes and collection progress are drawn inside the game, so you no longer
<kbd>Alt</kbd>+<kbd>Tab</kbd> back and forth while exploring.

Still actively updated ✿✿ヽ(°▽°)ノ✿

</div>

<img alt="Main window" src="Docs/images/readme/app-overview.png" />

## Download & Install

Go to **[Releases](https://github.com/kahvia-d/IMAO/releases/latest)** and download
**`IMao-v<version>-windows-x64.zip`**, then unzip it and run **`IMao-Launcher.exe`**.

> In that list, anything starting with `ext-` is the **KuroBBS sync browser extension**, not the
> program. The program package is `IMao-v…-windows-x64.zip`.

| Item | Requirement |
| :--- | :--- |
| OS | Windows 10 1809 (17763) or newer, **x64 only** |
| Game display | Must be **16:9**; coordinate OCR supports 1600×900 / 1920×1080 / 2560×1440 |
| Runtime | **No .NET installation needed** (the launcher is self-contained); the VC++ runtime ships with the package |
| Disk | About **1.5 GB** after extracting the full package (all map regions included) |

> Updates are small: **Settings → Check for updates** downloads only the shards that differ from your
> build — roughly 1.6 MB for a marker-data-only update, about 30 MB for a regular program update.
> You never re-download the whole package.

## Highlights

- **Real-time position sync.** Minimap feature matching is the primary source, backed by OCR of the
  in-game coordinate readout and big-map viewport solving. When tracking briefly drops, markers keep
  moving along the extrapolated path (about 7 units of error after one second).
- **Interactive big map and minimap navigation.** The overlay is drawn straight onto the game window:
  marker icons, count badges, overlap expansion, completed-marker display and **marker filtering** —
  the main window and the in-game tool palette share one filter state.
- **Automatic route planning.** Nearest-neighbour plus 2-opt local search, up to **500 targets per route**;
  measured P95 around **13.7 ms** at the 500-target scale. Preview, save, load, and **re-plan the
  remaining route live** while you walk.
- **One-key nearby collection.** Triggered within 15 minimap pixels by default (adjustable 5–120).
  A candidate list only appears when marker icons actually overlap; you can work through it
  continuously or collect a whole group at once.
- **Guide popup.** Marker details, illustrated guides, paging and zooming, with a separate always-on-top
  window for full-size images. Guides are cached online and **reading public guides requires no login**.
- **Automatic layered-map detection.** Inside caves and underground structures only your current floor's
  markers are shown; other floors appear dimmed with an up/down badge and surface markers hide
  themselves, then come back the moment you step out. Covers **57 layered maps / 90 floors**.
- **Per-region downloads.** Every region is a separate feature pack. Disable it, or delete the local copy
  to free disk space and download it again later. Offline import
  (`resources-<version>-offline.zip`) is supported too.
- **Xbox gamepad support.** Open the tool palette, the marker assistant, complete nearby markers,
  collect a whole group and zoom guide images — all without touching the keyboard.
- **KuroBBS progress sync.** With the [companion browser extension](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg),
  local completion records and your KuroBBS account are merged both ways (union only — nothing is ever un-checked).
- **A signed update chain.** Manifests are ECDSA P-256 signed, every package is SHA-256 verified and
  executables are rejected inside map packages. Updates only touch the program and map data —
  your completion records, routes, filters and personal settings are **never overwritten**.

<!-- markdownlint-disable -->

<details>
<summary><b>Screenshots</b> (click to expand, 5 images)</summary>

<br>

**Minimap navigation** — markers sit on the minimap while you explore, together with routes and count badges.

<img alt="Minimap navigation" src="Docs/images/readme/minimap-navigation.jpg" />

**Minimap detail** — overlapping icons show a count badge; a hotkey collects the nearest one in range.

<img alt="Minimap detail" src="Docs/images/readme/minimap-markers.jpg" />

**Interactive big map** — aligned with the in-game map; left click opens a guide, right click toggles completion.

<img alt="Interactive big map" src="Docs/images/readme/map-markers.jpg" />

**Route guidance** — after planning, targets are ordered and the next stop switches automatically.

<img alt="Route guidance" src="Docs/images/readme/map-route.jpg" />

**Automatic route planning** — rectangle select, free lasso or add what is on screen, then start guiding.

<img alt="Automatic route planning" src="Docs/images/readme/map-plan.jpg" />

</details>

<!-- markdownlint-restore -->

## Usage

### Quick start

1. Unzip the downloaded package and run `IMao-Launcher.exe`.
2. Launch Wuthering Waves, set the game to **16:9**, and make sure the **coordinate readout in the
   lower-left corner is clearly visible** — it is one of the tool's positioning sources.
3. Click **Start exploring** and wait for the status message in the lower-right corner to disappear;
   that means positioning succeeded.
4. **On a cold start, open the in-game big map once** so the tool can determine which region you are in.
   Until then the status bar says "open the big map once to determine your region". After that, region
   detection is automatic wherever you go.
5. Open Settings to enable minimap/big-map markers and the status bar, or download the regions you want.

### Default hotkeys

Every binding can be changed, disabled or reset in **Settings → Keyboard shortcuts**, and takes effect
immediately.

| Key | Action |
| :--- | :--- |
| <kbd>Z</kbd> | Complete the nearby marker (a candidate list appears first if icons overlap) |
| <kbd>F8</kbd> | Open / close the guide for the current target |
| <kbd>Q</kbd> | Record the two endpoints of a hand-drawn route on the big map |
| <kbd>PageUp</kbd> / <kbd>PageDown</kbd> | Previous / next guide image |
| <kbd>F9</kbd> | Start / stop exploring (same as the button on the home page) |
| <kbd>Shift</kbd> + left-drag | Temporary rectangle selection while picking route targets |
| <kbd>Esc</kbd> | Cancel the current gesture, otherwise go back to panning the map |

> <kbd>M</kbd>, <kbd>F10</kbd> and <kbd>Esc</kbd> are reserved and cannot be bound.
> <kbd>Ctrl</kbd>/<kbd>Alt</kbd>/<kbd>Shift</kbd>/<kbd>Win</kbd> combinations do not trigger these actions.

### Gamepad

Enable it first in **Settings → Xbox gamepad** (off by default). The tool does **not** capture gamepad
input, so the game's own action for the same button may fire at the same time.

| Context | Input | Action |
| :--- | :--- | :--- |
| In-game big map | <kbd>LB</kbd> | Open the map tool palette |
| In-game big map | <kbd>RB</kbd> | Open the standalone marker assistant |
| Exploring | <kbd>LB</kbd> → <kbd>B</kbd> | Complete the nearby marker |
| Exploring | <kbd>LB</kbd> → <kbd>X</kbd> | Open the guide for a nearby marker |
| Anywhere | <kbd>LB</kbd> + <kbd>Start</kbd> | Start / stop exploring |
| Marker details | Hold <kbd>A</kbd> for 0.6 s | Complete the current marker |
| Candidate list | Hold <kbd>X</kbd> for 0.6 s | Collect the whole group |
| Guide image | <kbd>X</kbd> / <kbd>LT</kbd> / <kbd>RT</kbd> / right stick | Zoom in / out / in / pan |

### KuroBBS progress sync extension

The tool can merge its progress with your [KuroBBS map](https://www.kurobbs.com/mc/map/) account:
markers completed only on KuroBBS are pulled into the local profile, markers completed only locally are
pushed back. It is a **union** — nothing is ever un-checked on either side.

**Edge Add-ons (recommended): [IMao KuroBBS Progress Sync](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg)**

On Chrome, load the same code by hand with the [manual install guide](Docs/KuroMapSyncInstall.md) —
`manifest.json` pins the release public key, so the unpacked build gets exactly the same extension id
as the store build (`ohmikf…`) and the desktop credentials and sync profiles carry over unchanged.

Once installed:

1. In the tool, open **Settings → KuroBBS marker progress sync** and click "Register/repair browser bridge".
2. Open and sign in to the KuroBBS map, click the extension icon, then "Connect desktop app".
3. Back in the tool, "Preview sync" to see the difference, then "Apply sync". With automatic sync on,
   completing a marker in game is pushed immediately and everything is reconciled every 10 minutes.

Credentials travel only over the browser's official Native Messaging channel to the local program, which
stores them encrypted with **DPAPI** under the current Windows user. The extension **does not read
cookies, does not touch other sites and makes no network requests of its own** — it only reads the
session the KuroBBS map page already holds.

### Supported regions

**13 regions, 51 sub-regions, about 23,800 markers** (527 icon types). Regions are grouped by country in
Settings and can be disabled, deleted and re-downloaded (**changes apply after a full restart**).

| Country | Regions |
| :--- | :--- |
| Huanglong (瑝珑) | Jinzhou (今州), Mengzhou (梦州) |
| Black Shores (黑海岸) | Black Shores Archipelago (黑海岸群岛), Tethys' Deep (泰缇斯之底), Time Rift Ruins (时隙废都) |
| Rinascita (黎那汐塔) | Ragunna (拉古那), Septimont (七丘), Lower Vault (下层金库), Avinoleum (阿维纽林), Fabricatorium (隐海试验场) |
| Roy's Icefield (罗伊冰原) | Icefield Surface (冰原地表), Lahai-Roi (拉海洛), Darkplain (黯原) |

A single region pack ranges from about 3 MB (Time Rift Ruins) to about 211 MB (Lahai-Roi). Regions that
ship with the program cost no extra download.

### FAQ and known limits

<details>
<summary><b>Click to expand</b></summary>

<br>

- **Why open the big map once on a cold start?**
  A region's identity can only be established by visual matching or by the first big-map solve; the
  in-game coordinate readout alone cannot decide which region you are in. It is a one-time step.
- **Why do markers disappear when tracking is lost?**
  When positioning data is unavailable, ambiguous or insufficient, the last trusted position is kept for
  up to 3 seconds while markers move along the extrapolated path. After 2 seconds without a new result
  markers stop being drawn, so they never sit in place and flicker.
- **Some places cannot be located from the minimap.**
  Dark, fine-contour art styles, large water surfaces and sparse reefs (parts of Chengxiao Mountain and
  Tethys, for example) have too few features and matching is conservatively rejected there.
  **Opening the big map once solves it** — big-map rendering matches the tiles the tool uses, so it
  localises very reliably.
- **Does the game have to be 16:9?**
  Yes. Coordinate OCR currently covers 1600×900 / 1920×1080 / 2560×1440 only. Other ratios or
  resolutions safely refuse coordinate recognition and fall back to visual matching alone.
- **What if two floors look almost identical?**
  The program treats them all as the current location instead of guessing one and mislabelling it as
  upstairs or downstairs.
- **When will new regions be available?**
  A new region needs four-point calibration, its own tile feature pack and in-game validation.
  Lower Vault, Darkplain and Time Rift Ruins are calibrated from local captures and usable today, while
  the full four-check verification and the 113-sample regression are still outstanding.
- **Does it cost frame rate?**
  The overlay only paints the minimap and status-bar area (about **3.3%** of a 2560×1440 screen), and
  screen capture is limited to the regions exploration needs — measured capture cost dropped from about
  13 ms to about 6 ms. If display or frame rate looks wrong, switch the overlay presentation mode or
  adjust the refresh interval in Settings.
- **Is it safe for my account?**
  The tool **only captures the visible game window and runs image matching**. It **does not read or write
  game memory and does not send any input to the game.** It cannot and does not claim anything about
  third-party anti-cheat behaviour — please judge for yourself before using it.

</details>

## Development

To build it yourself or contribute:

- [Reproducible build (Windows x64)](Docs/Build_zh-Hans.md)
- [Compile with CMake](Docs/Compile_en.md) / [CMake 编译说明](Docs/Compile_zh-Hans.md)
- [Documentation index](Docs/README.md) (build, updates, map data, UI, audit records)
- Layout: `IMao-Core/` (C++ native core: visual localisation, resource loading, IPC),
  `IMao-WinUI/` + `IMao-WinUI.Core/` (WinUI 3 shell: UI, updates, settings),
  `tools/UpdatePublisher/` + `scripts/` (signed manifests and the release pipeline),
  `BrowserExtensions/KuroMapSync/` (KuroBBS sync extension).

This project continues the work of
[IMao-Wuthering-Waves](https://github.com/Yepin2022/IMao-Wuthering-Waves).

## Credits

### Open-source libraries

- Image recognition: [OpenCV](https://github.com/opencv/opencv) (with opencv_contrib, using SURF)
- Text recognition: [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) / [Paddle Inference](https://www.paddlepaddle.org.cn/inference/master/guides/install/download_lib.html)
- Window capture: [Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
- Overlay UI: [ImGui](https://github.com/ocornut/imgui)
- C++ JSON: [nlohmann/json](https://github.com/nlohmann/json)
- WinUI components: [CommunityToolkit/Windows](https://github.com/CommunityToolkit/Windows),
  [microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml), [WinUIEx](https://github.com/dongle-the-gadget/WinUIEx)

### Data sources

- Map and marker data: [KuroBBS Wuthering Waves map](https://www.kurobbs.com/mc/map/)
- Tile imagery: KuroBBS public map endpoints

Thanks to everyone who developed, tested and reported issues — you are what makes this tool better! (\*´▽｀)ノノ

## Community

**User QQ group: `1109700733`**

Bugs, feature requests, or just somewhere to talk about Wuthering Waves — you are welcome to join.
You can also file an [issue](https://github.com/kahvia-d/IMAO/issues).

If this tool helped you, **please give it a Star** — it is the biggest support you can give us!

## Disclaimer

- This software is released under the [GNU General Public License v3.0](LICENSE) and is **free of charge**.
- It is open source and free, for learning and exchange only. If you run into a vendor charging for
  power-levelling with this software, the problems and consequences that arise are unrelated to this project.
- This project is not affiliated with, authorised, sponsored or endorsed by Kuro Games. Rights to game
  assets and map data belong to their respective owners.
- Map and marker data comes from public KuroBBS data. Please keep complying with the applicable terms of
  the upstream site and the game before redistributing or long-term hosting it.
- Any consequences of using this software are borne by the user.
