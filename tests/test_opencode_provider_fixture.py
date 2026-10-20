"""The OpenCode providers contract fixture: generated, private and byte-identical across copies."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / 'tests/fixtures/opencode-provider-catalog.json'
ANDROID = REPO / 'mobile/android/app/src/test/resources/opencode-provider-catalog.json'
GENERATOR = REPO / 'tests/fixtures/opencode-provider-catalog.py'
OUTPUTS = ('row', 'hgs_account', 'heartbeat_account', 'launch_account', 'stopped_account', 'stopped_hgs_account')
SECRETS = ('fixture-secret-key', '127.0.0.1', 'example.invalid', 'Authorization', 'baseURL', '"settings"',
           '"headers"', '"body"', 'cred_fixture', 'needs_auth', 'GROQ_API_KEY')


class ProviderCatalogFixture(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(FIXTURE.read_text())

    def test_the_generator_reproduces_both_committed_copies(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'fixture.json'
            subprocess.run([sys.executable, '-I', str(GENERATOR), str(out)], check=True, cwd=REPO)
            self.assertEqual(out.read_bytes(), FIXTURE.read_bytes())
        self.assertEqual(ANDROID.read_bytes(), FIXTURE.read_bytes())

    def test_outputs_never_carry_service_secrets(self):
        self.assertIn('fixture-secret-key', json.dumps(self.data['service']))
        self.assertIn('fixture-secret-key', json.dumps(self.data['account_models']['service']))
        outputs = [(key, self.data[key]) for key in OUTPUTS]
        outputs.append(('account_models.answer', self.data['account_models']['answer']))
        for key, value in outputs:
            text = json.dumps(value)
            for secret in SECRETS:
                self.assertNotIn(secret, text, key)

    def test_ascii_lengths_are_python_json_lengths(self):
        self.assertEqual(len(self.data['ascii_lengths']), 5)
        for case in self.data['ascii_lengths']:
            self.assertEqual(len(json.dumps(case['value'], ensure_ascii=True)), case['length'])

    def test_short_list_order_marks_and_kinds(self):
        options = self.data['row']['model_options']
        self.assertEqual([o['value'] for o in options], [
            'fixture/local', 'keyed/paid-model', 'groq/openai/gpt-oss-20b', 'fixture/alternate',
            'ollama/qwen3-coder:30b', 'opencode/glm-5.3-flash-free', 'opencode/big-pickle', 'groq/groq/compound'])
        self.assertEqual([o['value'] for o in options if o.get('default')], ['fixture/local'])
        self.assertEqual([o['value'] for o in options if o.get('favorite')],
                         ['keyed/paid-model', 'groq/openai/gpt-oss-20b'])
        self.assertNotIn('gone/missing', json.dumps(options))
        self.assertTrue(all(o['id'] == o['value'] for o in options))
        providers = self.data['row']['providers']
        self.assertEqual({p['id']: p['kind'] for p in providers},
                         {'opencode': 'free', 'groq': 'env', 'fixture': 'config', 'ollama': 'local',
                          'keyed': 'key', 'solo': 'oauth'})
        self.assertEqual([p.get('active_label') for p in providers], [None, None, None, None, None, 'Work'])

    def test_a_folder_catalog_marks_favorites_and_its_own_providers(self):
        case = self.data['account_models']
        answer = case['answer']
        self.assertEqual(answer['location'], case['location'])
        self.assertEqual([p['id'] for p in answer['providers'] if p.get('folder')], ['workspace'])
        self.assertEqual(answer['favorites'], [{'value': 'keyed/paid-model', 'available': True},
                                               {'value': 'groq/openai/gpt-oss-20b', 'available': True},
                                               {'value': 'gone/missing', 'available': False}])
        models = answer['models']
        self.assertEqual([m['value'] for m in models if m.get('favorite')],
                         ['groq/openai/gpt-oss-20b', 'keyed/paid-model'])
        self.assertEqual([m['value'] for m in models if m.get('default')], ['fixture/local'])
        self.assertNotIn('keyed/off', [m['value'] for m in models])
        self.assertIn('workspace/team-coder', [o['value'] for o in answer['model_options']])
        self.assertTrue(all(m['id'] == m['value'] for m in models + answer['model_options']))


if __name__ == '__main__':
    unittest.main()
