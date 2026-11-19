"""Run elevated while exploring. Requires psutil, pywin32 and the local PresentMon CLI.

python scripts/Measure-MapGpuLive.py --seconds 720 --block 60
Each live.jsonl line describes one interval. While recording, put 0..3 or stop in
the run directory's command.txt to steer a block or finish early. Bit 1 is the
minimap and bit 2 the big map. Changes are scoped to this connection.
"""
import argparse
import bisect
import collections
import csv
import ctypes
import datetime as dt
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time

import psutil
import win32gui
import win32pdh
import win32process


ROOT = Path(__file__).resolve().parents[1]
COUNTERS = ("minimapBatches", "minimapGpuBatches", "viewportExtractions", "viewportGpuExtractions")
ORDERS = ((0, 1, 3, 2), (1, 2, 0, 3), (2, 3, 1, 0), (3, 0, 2, 1))


def clock_ms():
    ticks, freq = ctypes.c_longlong(), ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks))
    ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(freq))
    return ticks.value * 1000 / freq.value


def unique_process(name):
    matches = [p for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == name.lower()]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one {name}, found {len(matches)}")
    return matches[0]


def request(pipe, command):
    pipe.write(command.encode("ascii"))
    line = pipe.readline(8192)
    if not line.endswith(b"\n"):
        raise IOError("IMao observation pipe disconnected")
    result = json.loads(line)
    if "error" in result:
        raise RuntimeError(result["error"])
    return result


def foreground_pid():
    window = win32gui.GetForegroundWindow()
    return win32process.GetWindowThreadProcessId(window)[1] if window else None


class GpuUsage:
    """Report busiest engine utilization for each process, keeping GPU engines separate."""
    def __init__(self):
        self.query = win32pdh.OpenQuery()
        self.counter = win32pdh.AddEnglishCounter(self.query, r"\GPU Engine(*)\Utilization Percentage")
        win32pdh.CollectQueryData(self.query)

    def sample(self, processes):
        win32pdh.CollectQueryData(self.query)
        engines = win32pdh.GetFormattedCounterArray(self.counter, win32pdh.PDH_FMT_DOUBLE)
        usage = collections.defaultdict(list)
        for name, value in engines.items():
            match = re.match(r"pid_(\d+)_", name)
            if match:
                usage[int(match[1])].append(value)
        return {name: round(max(usage[p.pid], default=0), 2) for name, p in processes.items()}


class RecognitionLog:
    def __init__(self):
        self.path = None
        self.offset = 0
        self.sample()

    def sample(self):
        path = Path(os.environ["LOCALAPPDATA"]) / "IMao-WinUI/Logs" / f"events-{dt.date.today():%Y%m%d}.jsonl"
        if not path.exists():
            return {"logMissing": 1}
        if path != self.path:
            self.offset = path.stat().st_size if self.path is None else 0
            self.path = path
        counts = collections.Counter()
        with path.open("rb") as f:
            if self.offset > path.stat().st_size:
                self.offset = 0
            f.seek(self.offset)
            for line in f:
                if not line.endswith(b"\n"):
                    break
                self.offset += len(line)
                event = json.loads(line)
                key = event.get("message")
                if key in ("coordinate-handoff", "coordinate-recognition", "map-ui-transition", "visual-localization-recovery",
                           "minimap-gpu-fallback", "viewport-gpu-fallback"):
                    counts[key] += 1
                elif key == "map-viewport-submit":
                    scope = re.search(r"\bscope=([\w-]+)", str(event.get("details", "")))
                    if scope:
                        counts[scope[1]] += 1
        return dict(counts)


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def qpc_scale(raw, reference_ms):
    # In PresentMon 2.5.1 the v1 CSV emits QPCTime in seconds even with --qpc_time_ms.
    scale = min((1, 1000), key=lambda factor: abs(raw * factor - reference_ms))
    if abs(raw * scale - reference_ms) > 60000:
        raise ValueError("Game frame timestamps do not match the observation clock")
    return scale


class LiveFrames:
    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.columns = None
        self.recent = collections.deque()

    def sample(self, now):
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            for line in stream:
                if not line.endswith(b"\n"):
                    break
                self.offset += len(line)
                values = next(csv.reader([line.decode("utf-8-sig")]))
                if self.columns is None:
                    self.columns = {name: i for i, name in enumerate(values)}
                    continue
                if values[self.columns["Dropped"]] == "0":
                    gap = float(values[self.columns["msBetweenDisplayChange"]])
                    raw = float(values[self.columns["QPCTime"]])
                    t = raw * qpc_scale(raw, now) + float(values[self.columns["msUntilDisplayed"]])
                    if gap > 0:
                        self.recent.append((t, values[self.columns["SwapChainAddress"]], gap))
        while self.recent and self.recent[0][0] < now - 3000:
            self.recent.popleft()
        if not self.recent:
            return None
        main = collections.Counter(chain for _, chain, _ in self.recent).most_common(1)[0][0]
        gaps = sorted(gap for _, chain, gap in self.recent if chain == main)
        return {"fps": round(len(gaps) * 1000 / sum(gaps), 1), "p99Ms": round(gaps[math.ceil(.99 * len(gaps)) - 1], 2)}


def summarize(folder):
    samples = [json.loads(line) for line in (folder / "live.jsonl").read_text(encoding="utf-8").splitlines()]
    starts = [x["startMs"] for x in samples]
    # msBetweenDisplayChange spans two displays. Attribute it only if the entire
    # interval belongs to eligible samples of one block, away from switching.
    chains = collections.Counter()
    with (folder / "frames.csv").open(encoding="utf-8-sig") as f:
        for frame in csv.DictReader(f):
            chains[frame["SwapChainAddress"]] += 1
    if not chains:
        raise RuntimeError("PresentMon recorded no game frames")
    if not samples:
        raise RuntimeError("No observation intervals recorded")
    main = chains.most_common(1)[0][0]
    groups = collections.defaultdict(list)
    scale = None
    with (folder / "frames.csv").open(encoding="utf-8-sig") as f:
        for frame in csv.DictReader(f):
            if frame["SwapChainAddress"] != main or frame["Dropped"] != "0":
                continue
            gap = float(frame["msBetweenDisplayChange"])
            raw = float(frame["QPCTime"])
            if scale is None:
                scale = qpc_scale(raw, starts[0])
            end = raw * scale + float(frame["msUntilDisplayed"])
            if gap <= 0:
                continue
            first, last = (bisect.bisect_right(starts, t) - 1 for t in (end - gap, end))
            if first < 0 or last < 0 or end > samples[last]["endMs"]:
                continue
            matched = samples[first:last + 1]
            if (matched[0]["block"] == matched[-1]["block"] and
                    all(s["eligible"] and s["mask"] == matched[0]["mask"] for s in matched) and
                    all(b["startMs"] - a["endMs"] < 2 for a, b in zip(matched, matched[1:]))):
                groups[matched[0]["mask"]].append(gap)
    result = {}
    for mask in range(4):
        rows = [s for s in samples if s["eligible"] and s["mask"] == mask]
        seconds = sum((s["endMs"] - s["startMs"]) / 1000 for s in rows)
        gaps = sorted(groups[mask])
        result[mask] = dict(seconds=round(seconds, 2), frames=len(gaps),
            fps=round(1000 * len(gaps) / sum(gaps), 2) if gaps else None,
            p99Ms=round(gaps[math.ceil(.99 * len(gaps)) - 1], 2) if gaps else None,
            over50PerMinute=round(60000 * sum(g > 50 for g in gaps) / sum(gaps), 2) if gaps else None,
            imaoCpu=round(sum((r["cpu"]["ui"] + r["cpu"]["core"]) * (r["endMs"] - r["startMs"])
                              for r in rows) / (seconds * 1000), 2) if rows else None,
            work={k: sum(r["work"][k] for r in rows) for k in COUNTERS})
    write_json(folder / "summary.json", result)
    return result


def record(args, folder):
    game = unique_process("Client-Win64-Shipping.exe")
    ui = unique_process("IMao-WinUI.exe")
    log = RecognitionLog()
    with open(rf"\\.\pipe\IMao.MapGpu.{ui.pid}", "r+b", buffering=0) as pipe:
        deadline = time.monotonic() + args.wait
        while True:
            try:
                original = request(pipe, "?")
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
            else:
                if original["coreState"] == "running" and foreground_pid() == game.pid:
                    break
            if time.monotonic() >= deadline:
                raise RuntimeError("Start exploration and bring the game to the foreground first")
            time.sleep(1)
        processes = {"game": game, "ui": ui, "core": psutil.Process(original["pid"])}
        write_json(folder / "session.json", dict(original=original, ui=ui.pid, game=game.pid,
                   seconds=args.seconds, blockSeconds=args.block, started=dt.datetime.now().astimezone().isoformat()))
        gpu = GpuUsage()
        try:
            with (folder / "frames.csv").open("wb") as frames, (folder / "presentmon.log").open("wb") as errors:
                pm = subprocess.Popen([str(args.presentmon), "--process_id", str(game.pid), "--output_stdout",
                    "--no_console_stats", "--v1_metrics", "--qpc_time_ms", "--session_name", f"IMaoMapGpu{os.getpid()}",
                    "--timed", str(args.seconds + 2), "--terminate_after_timed"], stdout=frames, stderr=errors,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    sample_session(args, folder, pipe, processes, gpu, log, pm)
                finally:
                    try:
                        if pm.poll() is None:
                            pm.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        pm.terminate()
                        pm.wait(timeout=5)
        finally:
            win32pdh.CloseQuery(gpu.query)
            write_json(folder / "restored.json", request(pipe, "R"))


def sample_session(args, folder, pipe, processes, gpu, log, pm):
    start = clock_ms()
    block = -1
    old = None
    frames = LiveFrames(folder / "frames.csv")
    with (folder / "live.jsonl").open("w", encoding="utf-8", buffering=1) as live:
        while (clock_ms() - start) / 1000 < args.seconds:
            if pm.poll() is not None:
                raise RuntimeError(f"PresentMon exited early ({pm.returncode}); see presentmon.log")
            next_block = int((clock_ms() - start) / (1000 * args.block))
            change = None
            if next_block != block:
                block = next_block
                change = ORDERS[(block // 4) % len(ORDERS)][block % 4]
            control = folder / "command.txt"
            if control.exists():
                command = control.read_text(encoding="utf-8-sig").strip()
                control.unlink()
                if command == "stop":
                    break
                if command not in ("0", "1", "2", "3"):
                    raise ValueError("command.txt accepts 0, 1, 2, 3 or stop")
                change = int(command)
            if change is not None:
                old = request(pipe, str(change))
                if old["mask"] != change:
                    raise RuntimeError("CoreHost did not acknowledge requested GPU mask")
                changed_at = clock_ms()
                before = changed_at
                prev_focus = foreground_pid()
                for p in processes.values():
                    p.cpu_percent()
                log.sample()
            time.sleep(min(1, max(0, args.seconds - (clock_ms() - start) / 1000)))
            now = request(pipe, "?")
            if now["pid"] != processes["core"].pid:
                raise RuntimeError("CoreHost restarted during the trial")
            after = clock_ms()
            focus = foreground_pid()
            eligible = (before - changed_at >= 3000 and now["mask"] == old["mask"] and
                        old["coreState"] == now["coreState"] == "running" and prev_focus == focus == processes["game"].pid)
            row = dict(startMs=before, endMs=after, time=dt.datetime.now().astimezone().isoformat(), block=block,
                mask=now["mask"], eligible=eligible, focused=focus == processes["game"].pid, localization=now["localization"],
                cpu={k: round(p.cpu_percent() / psutil.cpu_count(), 2) for k, p in processes.items()},
                rssMiB={k: round(p.memory_info().rss / 1048576, 1) for k, p in processes.items()},
                displayLast3s=frames.sample(after), busiestGpuEnginePercent=gpu.sample(processes),
                work={k: now[k] - old[k] for k in COUNTERS}, events=log.sample())
            live.write(json.dumps(row, ensure_ascii=False) + "\n")
            if now["mask"] != old["mask"]:
                changed_at = after
            old, before, prev_focus = now, after, focus


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=720)
    parser.add_argument("--block", type=int, default=60)
    parser.add_argument("--wait", type=int, default=0, help="Seconds to wait for exploration with the game in front")
    parser.add_argument("--presentmon", type=Path, default=ROOT / "out/perf/PresentMon-2.5.1.exe")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--analyze", type=Path)
    args = parser.parse_args()
    if args.analyze:
        print(json.dumps(summarize(args.analyze), indent=2))
    else:
        if args.seconds < 4 or args.block < 4 or not args.presentmon.is_file():
            parser.error("Recording requires a local PresentMon executable and intervals of at least four seconds")
        out = args.out or ROOT / "out/perf" / dt.datetime.now().strftime("map-gpu-live-%Y%m%d-%H%M%S")
        out.mkdir(parents=True, exist_ok=False)
        try:
            record(args, out)
            print(json.dumps(summarize(out), indent=2))
        except Exception as err:
            (out / "error.txt").write_text(str(err), encoding="utf-8")
            raise
