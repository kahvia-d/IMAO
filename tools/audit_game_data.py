"""Verify archived public game data and its runtime projection, without downloading."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path


def audit(root):
    data = root / "Assets/KuroMap"
    read = lambda path: json.loads(path.read_text(encoding="utf-8-sig"))
    manifest = read(data / "manifest.json")
    aliases = manifest["aliases"]
    failures, states = [], []

    def check(condition, message):
        if not condition:
            failures.append(message)

    def points(items):
        result = {}
        for item in items:
            key = aliases.get(item["id"], item["id"])
            for point in item["location"]:
                identity = (key, point["id"])
                check(identity not in result, f"duplicate point {identity}")
                normalized = dict(point)
                if "typeId" in normalized:
                    normalized["typeId"] = aliases.get(normalized["typeId"], normalized["typeId"])
                result[identity] = normalized
        return result

    new_scenes = {902: "LowerVault", 909: "Darkplain", 910: "TimeRiftRuins"}
    for state in manifest["states"]:
        state_id = state["state"]
        path = data / f"states/state-{state_id}.json"
        check(hashlib.sha256(path.read_bytes()).hexdigest() == state["sha256"], f"state {state_id} SHA-256")
        raw = read(path)
        expected = points(raw)
        check(len(raw) == state["itemTypes"] and len(expected) == state["points"], f"state {state_id} counts")
        scene = new_scenes.get(state_id) or state["runtime"]
        runtime = data / f"runtime/itemsData_{scene}.json" if state_id in new_scenes else root / f"IMao-Core/src/Resource/itemsData_{scene}.json"
        actual = points(read(runtime))
        check(expected == actual, f"state {state_id} runtime fields differ (including coordinates/floors)")
        floors = collections.Counter()
        for point in expected.values():
            check(all(isinstance(point[v], (int, float)) and not isinstance(point[v], bool) and math.isfinite(point[v]) for v in ("x", "y")), f"non-finite coordinate in {state_id}")
            floors[str(point.get("floorId"))] += 1
        states.append({"state": state_id, "scene": scene, "types": len(raw), "points": len(expected), "floors": dict(floors)})
    icons = read(data / "icon-manifest.json")["icons"]
    for name, relative in icons.items():
        path = (data / relative).resolve()
        check(path.is_relative_to(data.resolve()), f"icon path escapes archive: {name}")
        check(path.is_file() and path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), f"invalid PNG: {name}")
    return {"resourceVersion": manifest["resourceVersion"], "icons": len(icons), "states": states, "failures": failures, "passed": not failures}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = audit(args.root)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    raise SystemExit(0 if report["passed"] else 1)
