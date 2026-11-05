"""Keep a single deployable detector while testing interpolation with the flight baseline."""
import argparse
import os

from train import FLIGHT, OUT, PILOT, read


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate")
    parser.add_argument("fraction", type=float)
    args = parser.parse_args()
    assert 0 < args.fraction < 1
    os.environ.update(YOLO_CONFIG_DIR=str(PILOT / "settings"), YOLO_AUTOINSTALL="false")
    from ultralytics import YOLO
    import torch
    torch.set_num_threads(4)
    candidate = YOLO(str(OUT / args.candidate / "train/weights/last.pt"))
    initial = YOLO(read(FLIGHT / "flight-training.json")["weights"])
    a, b = initial.model.state_dict(), candidate.model.state_dict()
    assert a.keys() == b.keys()
    with torch.no_grad():
        for key,tensor in b.items():
            if tensor.is_floating_point():
                tensor.copy_(torch.lerp(a[key].float(),tensor.float(),args.fraction))
    folder = OUT / f"{args.candidate}-blend{round(args.fraction*100)}" / "train/weights"
    folder.mkdir(parents=True, exist_ok=False)
    candidate.save(str(folder / "last.pt"))
    YOLO(str(folder / "last.pt")).export(format="onnx", imgsz=(64,896), opset=12,
                                         dynamic=False, simplify=False, half=False, batch=1, device="cpu")


if __name__ == "__main__":
    main()
