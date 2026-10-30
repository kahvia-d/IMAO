import tempfile
import unittest
import wave
from pathlib import Path
import numpy as np
from transcribe import decode_mono

class AudioDecodeTests(unittest.TestCase):
    def test_stereo_48khz_is_resampled_without_time_shift(self):
        samples=np.zeros((48000,2),dtype=np.int16)
        samples[24000:28800]=10000
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'test.wav'
            with wave.open(str(path),'wb') as w:
                w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000);w.writeframes(samples.tobytes())
            decoded=decode_mono(path)
        self.assertEqual(np.float32,decoded.dtype)
        self.assertEqual((16000,),decoded.shape)
        self.assertLess(float(np.abs(decoded[:7800]).max()),0.001)
        self.assertGreater(float(decoded[8100:9400].mean()),0.2)

if __name__=='__main__':unittest.main()
