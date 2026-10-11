"""Isolated release mirror contract: no GitHub writes or product state."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import urllib.error
from email.message import Message

SCRIPTS=Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(SCRIPTS))
import update_feed as feed
spec=importlib.util.spec_from_file_location('publisher',SCRIPTS/'publish-update-feed.py')
publisher=importlib.util.module_from_spec(spec);spec.loader.exec_module(publisher)

COMMIT='a'*40


def archive(packages):
    raw=io.BytesIO()
    with tarfile.open(fileobj=raw,mode='w:gz') as output:
        for name,version in packages.items():
            pkgver,pkgrel=version.rsplit('-',1)
            data=f'pkgbase = {name}\n\tpkgver = {pkgver}\n\tpkgrel = {pkgrel}\npkgname = {name}\n'.encode()
            member=tarfile.TarInfo(f'aur/{name}/.SRCINFO');member.size=len(data);output.addfile(member,io.BytesIO(data))
    return raw.getvalue()


class FakeGitHub:
    def __init__(self,android=False,nightly=False,windows=False):
        self.calls=[];self.commit=COMMIT;self.rows=[];self.blobs={}
        if android:
            tag='android-dev-1';version='0.1.7';package=b'checked apk fixture';source=b'checked source fixture'
            self.info={'schema':1,'platform':'android','channel':'dev','version_name':version,'version_code':8,'package_name':'app.zerus.mobile','min_sdk':26,'signing_cert_sha256':'b'*64,'source_commit':COMMIT,'source_dirty':True,
                       'artifacts':[{'kind':'apk','name':'zerus.apk','sha256':hashlib.sha256(package).hexdigest(),'size':len(package)}, {'kind':'source','name':'zerus-source.tar.gz','sha256':hashlib.sha256(source).hexdigest(),'size':len(source)}],
                       'source_snapshot':{'name':'zerus-source.tar.gz','sha256':hashlib.sha256(source).hexdigest(),'size':len(source)}}
            payload={'zerus.apk':package,'zerus-source.tar.gz':source}
        else:
            version='0.37.0.r123.gaaaaaaa.n5' if nightly else '0.37.0';tag='nightly-1234' if nightly else 'v'+version
            packages={'zerus-ade-nightly-bin':version+'-1'} if nightly else {'zerus':version+'-1','zerus-ade-bin':version+'-1','zerus-git':'0.37.0.r91.g1234567-2'}
            self.info={'schema':1,'version':version,'source_commit':COMMIT,'architecture':'x86_64','distribution':'Arch Linux','native_agents_bundled':False,'aur_packages':list(packages)}
            if nightly:self.info.update(channel='nightly',product_version='0.37.0',revision_count=123,workflow_run_id=1234,workflow_run_number=5,release_tag=tag)
            payload={f'zerus-{version}-aur.tar.gz':archive(packages),f'zerus-{version}-arch-x86_64.tar.gz':b'binary tar'}
            if windows:
                self.info['windows_portable']=f'zerus-{version}-windows-x86_64.zip';payload[self.info['windows_portable']]=b'portable zip'
            if not nightly:payload[f'zerus-{version}-source.tar.gz']=b'source tar'
            for name,actual in packages.items():
                if name!='zerus-git':payload[f'{name}-{actual}-x86_64.pkg.tar.zst']=b'package '+name.encode()
        self.release={'id':10,'draft':False,'prerelease':android or nightly,'tag_name':tag,'published_at':'2026-10-09T12:00:00Z','html_url':f'https://github.com/ufna/zerus/releases/tag/{tag}'}
        self.payload=payload;self.seal()

    def seal(self):
        payload={**self.payload,'release-info.json':json.dumps(self.info).encode()}
        payload['SHA256SUMS']=''.join(hashlib.sha256(raw).hexdigest()+'  '+name+'\n' for name,raw in sorted(payload.items())).encode()
        self.release['assets']=[];self.blobs={}
        for index,(name,raw) in enumerate(payload.items(),1):
            self.blobs[index]=raw
            self.release['assets'].append({'id':index,'name':name,'size':len(raw),'digest':'sha256:'+hashlib.sha256(raw).hexdigest(),'state':'uploaded','browser_download_url':f'https://github.com/ufna/zerus/releases/download/{self.release["tag_name"]}/{name}'})
        self.rows=[self.release]

    def releases(self):return self.rows
    def tag_commit(self,tag):return self.commit
    def asset(self,row,path):
        self.calls.append(row['name']);raw=self.blobs[row['id']];path.write_bytes(raw)
        digest=hashlib.sha256(raw).hexdigest()
        feed.require(len(raw)==row['size'] and row.get('digest')=='sha256:'+digest,'Fake upstream byte mismatch')
        return digest


class MirrorTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)/'content';self.client=FakeGitHub()
    def tearDown(self):self.temp.cleanup()
    def run_mirror(self,execute=True,selector='desktop/stable'):
        return publisher.synchronize(self.client,self.root,[selector],execute)
    def test_dry_run_writes_nothing_and_projects_actual_vcs_package(self):
        self.run_mirror(False);self.assertFalse(self.root.exists())
        with tempfile.TemporaryDirectory() as stage:
            result,_=feed.validate_and_fetch(self.client,self.client.release,Path(stage),'desktop','stable')
        self.assertEqual(result['packages']['zerus-git'],'0.37.0.r91.g1234567-2')
    def test_immutable_noop_redownloads_no_assets_and_retains_pointer(self):
        self.root.mkdir();self.run_mirror();path=self.root/'updates/v1/desktop/stable.json';before=path.stat().st_mtime_ns
        self.client.calls.clear();self.run_mirror();self.assertEqual(self.client.calls,[]);self.assertEqual(path.stat().st_mtime_ns,before)
        self.assertTrue((self.root/'downloads/v0.37.0/SHA256SUMS').is_file())
    def test_modified_asset_hardfails_and_retains_feed(self):
        self.root.mkdir();self.run_mirror();path=self.root/'updates/v1/desktop/stable.json';before=path.read_bytes()
        asset=self.root/'downloads/v0.37.0/zerus-0.37.0-1-x86_64.pkg.tar.zst';asset.write_bytes(b'broken')
        with self.assertRaises(ValueError):self.run_mirror()
        self.assertEqual(path.read_bytes(),before)
    def test_digest_checksum_and_commit_mismatch(self):
        self.client.commit='c'*40
        with self.assertRaises(ValueError):self.run_mirror(False)
        self.client.commit=COMMIT;self.client.blobs[1]=b'mutated'
        with self.assertRaises(ValueError):self.run_mirror(False)
    def test_path_url_duplicate_and_missing_sums_rejected(self):
        for kind in ('path','url','duplicate','missing'):
            self.client=FakeGitHub();assets=self.client.release['assets']
            if kind=='path':assets[0]['name']='../secret'
            elif kind=='url':assets[0]['browser_download_url']='https://evil.example/package'
            elif kind=='duplicate':assets.append(copy.deepcopy(assets[0]))
            else:self.client.release['assets']=[a for a in assets if a['name']!='SHA256SUMS']
            with self.subTest(kind=kind),self.assertRaises(ValueError):self.run_mirror(False)
    def test_metadata_limit_and_disk_limit(self):
        self.client.release['assets'][0]['size']=feed.ASSET_LIMIT+1
        with self.assertRaises(ValueError):self.run_mirror(False)
        self.client=FakeGitHub()
        with self.assertRaises(ValueError):publisher.synchronize(self.client,self.root,['desktop/stable'],False,1)
        self.assertFalse(self.root.exists())
    def test_nightly_actual_provenance(self):
        self.client=FakeGitHub(nightly=True);self.run_mirror(False,'desktop/nightly')
        self.client.info['workflow_run_number']=6;self.client.seal()
        with self.assertRaises(ValueError):self.run_mirror(False,'desktop/nightly')
    def test_nightly_windows_zip_stays_on_github_and_out_of_the_v1_feed(self):
        self.client=FakeGitHub(nightly=True,windows=True);name=self.client.info['windows_portable']
        self.root.mkdir();self.run_mirror(True,'desktop/nightly')
        published=json.loads((self.root/'updates/v1/desktop/nightly.json').read_text())
        # Deployed GUIs reject unknown artifact kinds; the zip is neither listed nor mirrored.
        self.assertEqual({row['kind'] for row in published['artifacts']},{'linux-archive','arch-package'})
        self.assertNotIn(name,self.client.calls)
        self.assertFalse((self.root/'downloads/nightly-1234'/name).exists())
        self.assertTrue((self.root/'downloads/nightly-1234/SHA256SUMS').is_file())
    def test_windows_zip_digest_and_family_are_still_verified(self):
        self.client=FakeGitHub(nightly=True,windows=True);name=self.client.info['windows_portable']
        row=next(asset for asset in self.client.release['assets'] if asset['name']==name);row['digest']='sha256:'+'0'*64
        with self.assertRaisesRegex(ValueError,'digest mismatch'):self.run_mirror(False,'desktop/nightly')
        for info,selector in (({'windows_portable':'zerus-windows.zip'},'desktop/nightly'),({'windows_portable':None},'desktop/nightly')):
            self.client=FakeGitHub(nightly=True,windows=True);self.client.info.update(info);self.client.seal()
            with self.subTest(info=info),self.assertRaisesRegex(ValueError,'Unexpected'):self.run_mirror(False,selector)
        # Stable releases have no Windows asset yet: a zip there still fails closed.
        self.client=FakeGitHub(windows=True)
        with self.assertRaisesRegex(ValueError,'Unexpected desktop asset family'):self.run_mirror(False)
    def test_android_dirty_source_required_same_code_replacement_forbidden(self):
        self.client=FakeGitHub(android=True);self.root.mkdir();self.run_mirror(True,'android/dev')
        self.client.release['id']=11;self.client.release['published_at']='2026-10-10T12:00:00Z'
        with self.assertRaises(ValueError):self.run_mirror(False,'android/dev')
        self.client.info.pop('source_snapshot');self.client.seal()
        with self.assertRaises(ValueError):self.run_mirror(False,'android/dev')
    def test_channel_mismatch_fetches_only_metadata(self):
        self.client=FakeGitHub(android=True);self.client.info['channel']='nightly';self.client.seal()
        with self.assertRaises(ValueError):self.run_mirror(False,'android/dev')
        self.assertEqual(set(self.client.calls),{'SHA256SUMS','release-info.json'})
    def test_corrupt_newest_does_not_silently_fall_back(self):
        self.client=FakeGitHub(android=True);self.client.rows=[self.client.release,copy.deepcopy(self.client.release)]
        self.client.rows[1]['id']=9;self.client.blobs[1]=b'corrupt'
        with self.assertRaises(ValueError):self.run_mirror(False,'android/dev')
    def test_malicious_aur_member_not_extracted(self):
        path=Path(self.temp.name)/'aur.tar.gz'
        with tarfile.open(path,'w:gz') as archive:
            member=tarfile.TarInfo('aur/zerus/.SRCINFO');member.type=tarfile.SYMTYPE;member.linkname='/etc/passwd';archive.addfile(member)
        with self.assertRaises(ValueError):feed.aur_versions(path)
    def test_symlink_root_and_feed_rejected(self):
        self.root.symlink_to(Path(self.temp.name),target_is_directory=True)
        with self.assertRaises(ValueError):feed.private_root(self.root)
    def test_http_untrusted_and_malformed_ports(self):
        for url in ('http://api.github.com','https://evil.example','https://'+urllib.parse.quote('user')+'@'+'api.github.com','https://api.github.com:bad','https://api.github.com/#x'):
            with self.subTest(url=url),self.assertRaises(ValueError):feed.https(url,{'api.github.com'})
    def test_asset_redirect_never_forwards_credentials(self):
        client=feed.GitHub('private-token');requests=[]
        class Response(io.BytesIO):
            headers={'Content-Length':'2'}
        class Opener:
            def open(self,request,timeout):
                requests.append(request)
                if len(requests)==1:
                    headers=Message();headers['Location']='https://release-assets.githubusercontent.com/asset'
                    raise urllib.error.HTTPError(request.full_url,302,'redirect',headers,None)
                return Response(b'ok')
        client.opener=Opener();self.assertEqual(b''.join(client.stream('https://api.github.com/repos/ufna/zerus/releases/assets/1',2,api=True,accept='application/octet-stream')),b'ok')
        self.assertEqual(requests[0].get_header('Authorization'),'Bearer private-token');self.assertIsNone(requests[1].get_header('Authorization'))
    def test_metadata_change_same_release_refused(self):
        self.root.mkdir();self.run_mirror();path=self.root/'updates/v1/desktop/stable.json';original=json.loads(path.read_text());changed={**original,'sourceCommit':'c'*40}
        with self.assertRaises(ValueError):feed.check_previous(path,changed)

if __name__=='__main__':unittest.main()
