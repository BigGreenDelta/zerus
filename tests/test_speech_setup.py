"""Private optional-service installer contracts; never call the real supervisor."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('speech_installer', REPO/'services/speech/install-user-service.py')
INSTALLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALLER)


class SpeechInstaller(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='zerus-speech-installer-')
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.config = self.home/'config'
        self.env = patch.dict(os.environ, {'HOME':str(self.home), 'HGS_CONFIG_DIR':str(self.config)})
        self.env.start(); self.addCleanup(self.env.stop)
        self.unit = self.home/'.config/systemd/user/zerus-speech.service'

    def invoke(self, arguments):
        with patch.object(sys, 'argv', ['install-user-service.py', *arguments]), \
                patch.object(INSTALLER.subprocess, 'run') as run, \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            INSTALLER.main()
        return [call.args[0] for call in run.call_args_list]

    def test_managed_profiles_need_no_default_json_or_config_overwrite(self):
        private = self.config/'speech-profiles'
        private.mkdir(parents=True, mode=0o700)
        marker = private/'profiles.sqlite3'
        marker.write_bytes(b'Existing managed store belongs to the CLI')
        calls = self.invoke(['--python', sys.executable, '--start'])
        self.assertFalse((self.config/'speech.json').exists())
        self.assertEqual(marker.read_bytes(), b'Existing managed store belongs to the CLI')
        unit = self.unit.read_text()
        self.assertIn(' -m zerus_speech --config '+INSTALLER.unit_argument(self.config/'speech.json'), unit)
        self.assertEqual(calls, [[str(Path(sys.executable).absolute()), '-c', 'import zerus_speech'],
            ['systemctl','--user','daemon-reload'],
            ['systemctl','--user','enable','--now','zerus-speech.service']])

    def test_explicit_existing_config_is_preserved_and_start_is_opt_in(self):
        custom = self.home/'custom config.json'
        original = '{"schema":1,"profiles":[],"analytics_enabled":false}'
        custom.write_text(original)
        calls = self.invoke(['--python',sys.executable,'--config',str(custom)])
        self.assertEqual(custom.read_text(), original)
        self.assertIn('--config '+INSTALLER.unit_argument(custom), self.unit.read_text())
        self.assertEqual(calls[-1], ['systemctl','--user','daemon-reload'])
        self.assertEqual(len(calls), 2)

    def test_help_and_explicit_missing_config_have_no_side_effects(self):
        for arguments, code in [(['--help'],0),
                (['--python',sys.executable,'--config',str(self.home/'missing.json')],2),
                (['--python',str(self.home/'missing-python')],2)]:
            with self.subTest(arguments=arguments), \
                    patch.object(INSTALLER.subprocess,'run') as run, \
                    patch.object(sys,'argv',['install-user-service.py',*arguments]), \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exited:
                    INSTALLER.main()
                self.assertEqual(exited.exception.code, code)
                run.assert_not_called()
                self.assertEqual(list(self.home.iterdir()), [])

    def test_existing_invalid_default_config_is_not_silently_ignored(self):
        (self.config/'speech.json').mkdir(parents=True)
        with self.assertRaises(SystemExit) as exited:
            self.invoke(['--python',sys.executable])
        self.assertEqual(exited.exception.code, 2)
        self.assertFalse(self.unit.exists())


if __name__ == '__main__':
    unittest.main()
