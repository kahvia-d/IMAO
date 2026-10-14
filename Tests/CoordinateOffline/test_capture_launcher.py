"""Run the real PS5 launcher against a native test-only event process."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = os.environ.get('IMAO_CAPTURE_LAUNCHER_FIXTURE')


@unittest.skipUnless(FIXTURE, 'Set IMAO_CAPTURE_LAUNCHER_FIXTURE to the built native fixture')
class LauncherContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.tool = self.root/'tool'
        self.tool.mkdir()
        shutil.copyfile(FIXTURE, self.tool/'IMaoCoordinateSampleCapture.exe')
        self.env = os.environ.copy()

    def tearDown(self):
        self.launch('-Stop')
        self.temp.cleanup()

    def launch(self, *args):
        # File sinks avoid an unrelated inherited pipe keeping communicate() open
        # after PS5 has exited while its intentionally detached child is alive.
        name = uuid.uuid4().hex
        out, err = self.root/(name+'.out'), self.root/(name+'.err')
        with out.open('wb') as stdout, err.open('wb') as stderr:
            result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(ROOT/'scripts/Start-CoordinateSampleCapture.ps1'),
                                     '-ToolRoot', str(self.tool), *args], env=self.env,
                                    stdout=stdout, stderr=stderr, timeout=15)
        result.stdout = out.read_text(errors='replace')
        result.stderr = err.read_text(errors='replace')
        return result

    def test_trailing_separator_preserves_argument_boundaries(self):
        output = self.root/'samples with space'
        result = self.launch('-OutputRoot', str(output)+'\\')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((output/'fixture-arguments-ok.txt').is_file())
        stopped = self.launch('-Stop')
        self.assertEqual(stopped.returncode, 0, stopped.stderr)

    def test_stop_reports_native_failure_and_repeated_stop_preserves_failure(self):
        self.env['IMAO_FIXTURE_FAIL'] = '1'
        result = self.launch('-OutputRoot', str(self.root/'samples'))
        self.assertEqual(result.returncode, 0, result.stderr)
        stopped = self.launch('-Stop')
        self.assertNotEqual(stopped.returncode, 0)
        self.assertIn('intentional fixture failure', stopped.stderr)
        again = self.launch('-Stop')
        self.assertNotEqual(again.returncode, 0)
        self.assertIn('intentional fixture failure', again.stderr)

    def test_stale_pid_does_not_block_a_new_start_or_signal_other_process(self):
        state = {'processId': os.getpid(), 'startedAtUtc': 'old', 'outputRoot': str(self.root),
                 'stdout': str(self.root/'old.stdout'), 'stderr': str(self.root/'old.stderr')}
        (self.root/'old.stdout').write_text('Finished: 0 samples\n')
        (self.root/'old.stderr').write_text('')
        (self.tool/'capture-process.json').write_text(json.dumps(state))
        stopped = self.launch('-Stop')
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        result = self.launch('-OutputRoot', str(self.root/'samples'))
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
