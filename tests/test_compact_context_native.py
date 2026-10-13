"""Real Claude compaction with a localhost-only model and native completion hooks."""
import json, subprocess, time, unittest, uuid
from pathlib import Path
import test_input
from test_clear_context_native import NativeClearContext
from test_claude_questions_native import CLAUDE

@unittest.skipUnless(CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for native compaction')
class NativeCompactContext(unittest.TestCase):
    setUp=NativeClearContext.setUp
    script=NativeClearContext.script
    write_record=NativeClearContext.write_record
    wait_for=NativeClearContext.wait_for
    screen=NativeClearContext.screen
    hook=NativeClearContext.hook
    inspect=NativeClearContext.inspect
    key=NativeClearContext.key
    tmux=NativeClearContext.tmux
    hook_events=['SessionStart','PreCompact','PostCompact','StopFailure']
    def test_native_compaction_reports_success(self):
        self.key('Escape');self.key('Escape');time.sleep(.5)
        if 'No, keep bypass permissions' in self.screen():self.key('Enter');time.sleep(.3)
        self.record.update(phase='idle',activity='idle',active_tools={});self.write_record()
        payload=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id)
        result=subprocess.run([str(test_input.HGS),'compact-context',self.name,'--json'],input=json.dumps(payload),env=self.env,text=True,capture_output=True,timeout=12)
        self.assertEqual(result.returncode,0,result.stderr+'\n'+self.screen())
        self.wait_for(lambda:json.loads(self.record_path.read_text()).get('compact_context_request',{}).get('status')=='completed')
        current=json.loads(self.record_path.read_text())
        self.assertEqual(current['compact_context_request']['request_id'],payload['request_id'])
        self.assertEqual(current['conversation_id'],self.conversation_id)
        self.assertEqual(current['phase'],'idle')
        # PostCompact completes the request before Claude redraws its summary.
        self.wait_for(lambda:'compacted' in self.screen().lower())
        def compact_boundary():
            history=[json.loads(line) for line in Path(current['transcript']).read_text().splitlines()]
            return next((event for event in reversed(history) if event.get('subtype')=='compact_boundary'),None)
        boundary=self.wait_for(compact_boundary)
        fresh=self.inspect()['session_usage']
        self.assertEqual(fresh['prompt_cache']['status'],'unknown')
        self.assertEqual(fresh['prompt_cache']['source'],'native_compaction')
        self.assertEqual(fresh.get('context',{}).get('used'),boundary['compactMetadata'].get('postTokens'))
        # Completion authorizes exactly the captured message in this run.
        message=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,
                     expected_conversation_id=self.conversation_id,expected_compaction_id=payload['request_id'],
                     text='Continue after the verified native compact.',attachments=[])
        sent=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],input=json.dumps(message),env=self.env,text=True,capture_output=True,timeout=12)
        self.assertEqual(sent.returncode,0,sent.stderr+'\n'+self.screen())
        self.wait_for(lambda:any(e.get('type')=='user' and e.get('message',{}).get('content')==message['text']
            for e in map(json.loads,Path(current['transcript']).read_text().splitlines())))
        replay=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],input=json.dumps(message),env=self.env,text=True,capture_output=True,timeout=12)
        self.assertEqual(replay.returncode,0,replay.stderr)
        events=list(map(json.loads,Path(current['transcript']).read_text().splitlines()))
        self.assertEqual(sum(e.get('type')=='user' and e.get('message',{}).get('content')==message['text'] for e in events),1)
if __name__=='__main__':unittest.main()
