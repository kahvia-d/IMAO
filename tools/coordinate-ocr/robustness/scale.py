"""Probe intact XY at changed scale independently of the centered, clipped-sign probe."""
import json
import cv2
from freeze import EVIDENCE
from train import OUT, read


def main():
    folder = OUT / "native-evaluation"
    rows = []
    for item in read(EVIDENCE / "ocr-sensitivity-20261024/visual-labels-v2.json")["frames"]:
        index = item["frame"]
        path = EVIDENCE / f"client-crop-fix-20261024/frames/{index}.png"
        frame = cv2.imread(str(path))
        for factor in (.85, .9, 1.1, 1.15):
            w,h = round(456*factor), round(25*factor)
            top = 1092 + (25-h)//2
            crop = frame[top:top+h,25:25+w]
            destination = folder / f"scale-left-{index}-{factor}.png"
            assert cv2.imwrite(str(destination),crop)
            rows.append(dict(id=f"repro-{index}/scale-left-{factor}",crop=str(destination),
                             expected=item.get("xy"), calibrated=False))
    (folder / "scale-left.json").write_text(json.dumps(rows))


if __name__ == "__main__":
    main()
