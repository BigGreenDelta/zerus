"""Managed cleanup and archive restoration against private Git/tmux/fake agents."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

import test_pause

test_pause.HGS=Path(os.environ.get('HGS_TEST_BIN',Path(__file__).resolve().parents[1]/'target/debug/hgs')).resolve()


@unittest.skipUnless(test_pause.TMUX and shutil.which('git'), 'private tmux and Git required')
class WorktreeLifecycle(test_pause.Harness):
    def setUp(self):
        super().setUp();self.repo=self.project
        self.git('init','-q','-b','main',str(self.repo))
        (self.repo/'tracked').write_text('original')
        self.git('-C',str(self.repo),'add','tracked');self.commit(self.repo,'initial')
        self.project=self.root/'managed tree'
        self.hgs('worktrees','create','--path',str(self.repo),'--destination',str(self.project),'--branch','session-work','--json')
        (self.root/'cfg/projects.local').write_text('p='+str(self.project)+'\n')

    def git(self,*args):
        return subprocess.check_output(['git',*args],env=self.env,stderr=subprocess.PIPE,text=True).strip()

    def commit(self,folder,message):
        self.git('-C',str(folder),'-c','user.name=Fixture','-c','user.email=fixture@example.test','commit','-qm',message)

    def archives(self):
        return [json.loads(p.read_text()) for p in (self.root/'state/archives').glob('*.json')]

    def command(self,*args):
        result=subprocess.run([str(test_pause.HGS),*args],env=self.env,text=True,capture_output=True,timeout=40)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        outcomes=[json.loads(line.split(' ',1)[1]) for line in result.stderr.splitlines() if line.startswith('ZERUS_WORKTREE_CLEANUP ')]
        self.assertEqual(len(outcomes),1,result.stderr)
        return outcomes[0]

    def test_last_terminate_removes_managed_integrated_tree_and_explicit_restore_keeps_identity(self):
        name=self.start('claude');identity=self.binding(name)['conversation_id']
        result=self.command('terminate',name);self.assertEqual(result['status'],'removed',result);self.assertFalse(self.project.exists())
        archive=self.archives()[0];self.assertEqual(archive['conversation_id'],identity)
        self.assertTrue(self.git('-C',str(self.repo),'show-ref','refs/heads/session-work'))
        self.hgs('resume',name,'--archive',archive['archive_id'],'--dry-run');self.assertFalse(self.project.exists())
        self.hgs('resume',name,'--archive',archive['archive_id'],'-d')
        self.wait(lambda:self.binding(name).get('conversation_id')==identity and not self.binding(name).get('expected_id'))
        self.assertTrue(self.project.is_dir());self.assertEqual((self.project/'tracked').read_text(),'original')

    def test_manual_archive_runs_cleanup_but_pause_alone_does_not(self):
        name=self.start('codex');self.hgs('pause',name);self.assertTrue(self.project.exists())
        result=self.command('archive',name);self.assertEqual(result['status'],'removed',result);self.assertFalse(self.project.exists())

    def test_other_session_keeps_folder_until_last_termination(self):
        first=self.start('claude','first');second=self.start('claude','second')
        result=self.command('terminate',first);self.assertEqual(result['status'],'kept',result);self.assertTrue(self.project.exists());self.assertTrue(self.binding(second))
        result=self.command('terminate',second);self.assertEqual(result['status'],'removed',result)
        self.assertEqual(len(self.archives()),2)

    def test_unmerged_dirty_and_ignored_work_are_never_automatically_removed(self):
        for kind in ('unmerged','dirty','ignored'):
            with self.subTest(kind=kind):
                if kind=='unmerged':
                    (self.project/'extra').write_text('work');self.git('-C',str(self.project),'add','extra');self.commit(self.project,'not integrated')
                elif kind=='dirty':(self.project/'tracked').write_text('unsaved')
                else:
                    self.git('-C',str(self.project),'reset','--hard','main')
                    (self.repo/'.git/info/exclude').write_text('local-notes\n');(self.project/'local-notes').write_text('private data')
                name=self.start('claude',kind);result=self.command('terminate',name)
                self.assertEqual(result['status'],'kept',result);self.assertTrue(self.project.exists())

    def test_missing_nonce_or_recreated_checkout_does_not_count_as_managed(self):
        metadata=Path(self.git('-C',str(self.project),'rev-parse','--absolute-git-dir'))
        (metadata/'zerus-owner.json').unlink()
        name=self.start('claude');result=self.command('terminate',name)
        self.assertEqual(result['status'],'kept',result);self.assertIn('verified Zerus-created',result['reason']);self.assertTrue(self.project.exists())

    def test_recreated_same_path_and_branch_cannot_reuse_old_ownership(self):
        self.git('-C',str(self.repo),'worktree','remove',str(self.project));self.git('-C',str(self.repo),'worktree','add','-q',str(self.project),'session-work')
        name=self.start('claude');result=self.command('terminate',name);self.assertEqual(result['status'],'kept',result);self.assertTrue(self.project.exists())

    def test_same_tree_integration_can_clean_without_deleting_the_original_branch(self):
        (self.project/'same').write_text('integrated');self.git('-C',str(self.project),'add','same');self.commit(self.project,'feature version')
        head=self.git('-C',str(self.project),'rev-parse','HEAD');(self.repo/'same').write_text('integrated');self.git('-C',str(self.repo),'add','same');self.commit(self.repo,'squashed version')
        name=self.start('claude');result=self.command('terminate',name);self.assertEqual(result['status'],'removed',result)
        self.assertEqual(self.git('-C',str(self.repo),'rev-parse','session-work'),head)

    def test_restore_refuses_changed_branch_without_creating_folder_or_starting_agent(self):
        name=self.start('claude');result=self.command('terminate',name);self.assertEqual(result['status'],'removed',result)
        archive=self.archives()[0];self.git('-C',str(self.repo),'-c','user.name=Fixture','-c','user.email=fixture@example.test','commit','--allow-empty','-qm','new main')
        self.git('-C',str(self.repo),'branch','-f','session-work','main')
        self.hgs('resume',name,'--archive',archive['archive_id'],'-d',rc=1)
        self.assertFalse(self.project.exists());self.assertEqual(len(self.archives()),1);self.assertFalse(self.binding(name))

    def test_restore_never_follows_a_replaced_destination_parent(self):
        parent=self.root/'parent';parent.mkdir();self.project=parent/'linked'
        self.hgs('worktrees','create','--path',str(self.repo),'--destination',str(self.project),'--branch','nested-session','--json')
        (self.root/'cfg/projects.local').write_text('p='+str(self.project)+'\n')
        name=self.start('claude');result=self.command('terminate',name);self.assertEqual(result['status'],'removed',result);archive=self.archives()[0]
        parent.rename(self.root/'original-parent');other=self.root/'other';other.mkdir();parent.symlink_to(other,target_is_directory=True)
        self.hgs('resume',name,'--archive',archive['archive_id'],'-d',rc=1)
        self.assertFalse((other/'linked').exists());self.assertFalse(self.binding(name))


if __name__=='__main__':unittest.main()
