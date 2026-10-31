"""Guard the real publisher wiring, which header-only motion tests cannot cover."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MinimapPublicationPolicy(unittest.TestCase):
    def test_no_short_window_position_extrapolator(self):
        for name in ("App/App.cpp", "App/App.h",
                     "Coordinate/IdentifyWorldCoordinates/CoordinateTrust.h"):
            code = (ROOT / "IMao-Core/src" / name).read_text(encoding="utf-8-sig")
            for forbidden in ("PredictAt(", "predictedShadow", "drawnMinimapROC",
                              "predicted-outlier", "hidden-stale"):
                with self.subTest(file=name, token=forbidden):
                    self.assertTrue(forbidden not in code, f"{name}: forbidden {forbidden}")

    def test_production_projects_every_layer_using_published_pose(self):
        code = (ROOT / "IMao-Core/src/App/App.cpp").read_text(encoding="utf-8-sig")
        self.assertTrue("ProjectMinimapLayout(frame," in code, "missing unified publisher call")
        start = code.index("ProjectMinimapLayout(frame,")
        call = code[start:code.index(";", start)]
        self.assertIn("frame.minimapMotion.center", call)
        self.assertIn("frame.minimapMotion.scene", call)
        self.assertLess(code.index("RetainAcceptedMinimapAnchor(frame,"), start)
        self.assertNotIn("ProjectReadoutMinimapLayout", code)

    def test_no_alternate_ocr_animation(self):
        for name in ("Runtime/ImageAnchoredOverlay.h", "ImguiDraw/ImGuiOverWindows.cpp"):
            code = (ROOT / "IMao-Core/src" / name).read_text(encoding="utf-8-sig")
            self.assertTrue("ReadoutDisplayMode" not in code, name)
            self.assertTrue("ReadoutDisplayMotion" not in code, name)


if __name__ == "__main__":
    unittest.main()
