"""An agent's guarded send names its session in one envelope; other sends stay exact."""
import hashlib
import json
import os
import subprocess
import unittest
import uuid
import xml.etree.ElementTree as ET
from urllib.parse import parse_qs, urlsplit

import test_input


@unittest.skipIf(os.name == 'nt', 'POSIX native-process ancestry fixture')
class SenderEnvelope(unittest.TestCase):
    setUp = test_input.InputTransport.setUp
    tearDown = test_input.InputTransport.tearDown
    script = test_input.InputTransport.script
    tmux = test_input.InputTransport.tmux
    wait_for = test_input.InputTransport.wait_for
    write_record = test_input.InputTransport.write_record
    payload = test_input.InputTransport.payload
    send = test_input.InputTransport.send
    received = test_input.InputTransport.received

    def sender(self, owned=True):
        """A binding whose native agent is, or is not, this test's own process."""
        origin = dict(self.record, name='claude/graph/source', agent='claude',
                      run_id=str(uuid.uuid4()), conversation_id=str(uuid.uuid4()))
        if owned:
            origin['pid'] = os.getpid()
            origin['process_start'] = subprocess.check_output(
                ['ps', '-p', str(os.getpid()), '-o', 'lstart='], env=dict(self.env, LC_ALL='C', TZ='UTC'),
                text=True).strip()
        path = self.state / (hashlib.sha256(origin['name'].encode()).hexdigest() + '.json')
        path.write_text(json.dumps(origin))
        self.env.update(HGS_SESSION=origin['name'], HGS_RUN_ID=origin['run_id'])
        return origin

    def delivered(self, before=b''):
        self.wait_for(lambda: self.received().endswith(b'\r') and len(self.received()) > len(before))
        sent = self.received()[len(before):]
        self.assertTrue(sent.startswith(b'\x1b[200~') and sent.endswith(b'\x1b[201~\r'), sent)
        return sent[len(b'\x1b[200~'):-len(b'\x1b[201~\r')].decode()

    def test_an_agents_send_carries_its_reference_and_keeps_its_identity(self):
        origin = self.sender()
        text = 'The patch is ready.\n\nReview it, please.'
        payload = self.payload(text=text)
        receipt = self.send(payload)
        delivered = self.delivered()
        head, body = delivered.split('\n', 1)
        self.assertEqual(body, text + '\n</zerus-message>')
        self.assertTrue(head.startswith('<zerus-message from="claude/graph/source" host="test" reference="zerus://'), head)
        frame = ET.fromstring(delivered)
        self.assertEqual(frame.text, '\n' + text + '\n')
        reference = frame.attrib['reference']
        fields = {key: value[0] for key, value in parse_qs(urlsplit(reference).query).items()}
        self.assertEqual((fields['name'], fields['run'], fields['conversation'], fields['host']),
                         (origin['name'], origin['run_id'], origin['conversation_id'], 'test'))
        # The receipt and its request identity cover the delivered text.
        self.assertEqual(receipt['text'], delivered)
        self.assertEqual(receipt['submitted_text'], delivered)
        # The recipient can read the sender through the copied reference.
        inspected = subprocess.run([str(test_input.HGS), 'inspect', reference], env=self.env, text=True,
                                   capture_output=True, timeout=10)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        self.assertEqual(json.loads(inspected.stdout)['name'], origin['name'])
        # A deliberate retry returns the same receipt and types nothing again.
        before = self.received()
        self.assertEqual(self.send(payload), receipt)
        self.assertEqual(self.received(), before)
        self.assertIn('already used', self.send(dict(payload, text='Another text'), success=False))
        self.assertEqual(self.received(), before)

    def test_an_agents_xml_body_round_trips_literal_markup_and_entities(self):
        self.sender()
        text = 'Snow 雪 & <tag> "quoted" </zerus-message> &amp; &lt; &#65;'
        payload = self.payload(text=text)
        receipt = self.send(payload)
        delivered = self.delivered()
        self.assertIn('&lt;/zerus-message&gt;', delivered)
        self.assertEqual(ET.fromstring(delivered).text, '\n' + text + '\n')
        self.assertEqual(receipt['text'], delivered)
        before = self.received()
        self.assertEqual(self.send(payload), receipt)
        self.assertEqual(self.received(), before)

    def test_an_owner_send_stays_byte_identical(self):
        text = 'Owner text <zerus-message from="x/y">not mine</zerus-message>'
        receipt = self.send(self.payload(text=text))
        self.assertEqual((self.delivered(), receipt['text']), (text, text))

    def test_an_environment_label_alone_adds_no_envelope(self):
        self.sender(owned=False)
        receipt = self.send(self.payload(text='Owner text'))
        self.assertEqual((self.delivered(), receipt['text']), ('Owner text', 'Owner text'))

if __name__ == '__main__':
    unittest.main()
