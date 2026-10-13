"""Allowlisted native session creation and privacy-minimal local catalogs."""
import json
import re
from pathlib import PurePosixPath
from .context import bounded
from .projects import normalize
from . import worktrees
from .accounts import MAX_CATALOG_BYTES, model_options

OPERATIONS = frozenset({'catalog','dirs','launch'}) | worktrees.OPERATIONS
AGENTS = frozenset({'codex','claude','kimi','dsh','opencode'})


def validate(operation, payload, request_id):
    if operation in worktrees.OPERATIONS:
        return worktrees.validate(operation, payload, request_id)
    fields = {'catalog': {'request_id'}, 'dirs': {'request_id','path'},
              'launch': {'request_id','agent','directory','tag'}}[operation]
    extras = {'account_id','model','swarm_id','project_id','project_folder_id','add_folder','worktree_folder_id','worktree_common_dir'} if operation=='launch' else set()
    if not isinstance(payload, dict) or not fields <= set(payload) or set(payload)-fields-extras or payload.get('request_id')!=request_id:
        raise ValueError('invalid scoped catalog/launch fields')
    if operation=='dirs':
        path=bounded(payload['path'],4096)
        if path!='~' and not PurePosixPath(path).is_absolute(): raise ValueError('directory must be absolute or home')
    if operation=='launch':
        if not isinstance(payload['agent'],str) or payload['agent'] not in AGENTS: raise ValueError('unsupported native provider')
        directory=bounded(payload['directory'],4096)
        if not PurePosixPath(directory).is_absolute(): raise ValueError('launch directory must be absolute')
        tag=bounded(payload['tag'],120)
        if len(tag.encode('utf-8'))>120 or tag.strip()!=tag or any(c in '/\\:.' for c in tag): raise ValueError('invalid native session tag')
        if 'account_id' in payload:
            value=bounded(payload['account_id'],80)
            if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_-]*',value): raise ValueError('invalid native account identifier')
        if 'model' in payload:
            model = bounded(payload['model'],256)
            provider, separator, identity = model.partition('/')
            if (payload['agent'] != 'opencode' or payload.get('account_id') != 'native-opencode'
                    or len(model.encode('utf-8')) > 256 or not separator or not identity
                    or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}',provider)
                    or any(c.isspace() for c in model)):
                raise ValueError('model selection requires the native OpenCode account and provider/model')
        if {'worktree_folder_id','worktree_common_dir'} & set(payload):
            if not {'swarm_id','project_id','add_folder','worktree_folder_id','worktree_common_dir'} <= set(payload) or payload['add_folder'] is not False or 'project_folder_id' in payload:
                raise ValueError('worktree placement requires a distinct current project anchor')
            anchor=bounded(payload['worktree_folder_id'],128)
            if anchor.strip()!=anchor or len(anchor.encode('utf-8'))>128: raise ValueError('invalid worktree anchor identifier')
            worktrees.absolute(payload['worktree_common_dir'])
        project_fields = {'swarm_id','project_id','project_folder_id','add_folder'} & set(payload)
        if project_fields:
            if not {'swarm_id','project_id','add_folder'} <= set(payload) or type(payload['add_folder']) is not bool:
                raise ValueError('project launch requires exact swarm/project and folder intent')
            bounded(payload['swarm_id'],128); project=bounded(payload['project_id'],128)
            if project.strip()!=project or len(project.encode('utf-8'))>128: raise ValueError('invalid project identifier')
            if 'project_folder_id' in payload:
                folder=bounded(payload['project_folder_id'],128)
                if folder.strip()!=folder or len(folder.encode('utf-8'))>128: raise ValueError('invalid project folder identifier')
                if payload['add_folder']: raise ValueError('an existing folder cannot also be added')


def catalog(raw):
    if not isinstance(raw,dict) or not isinstance(raw.get('profiles'),list) or len(raw['profiles'])>1000:
        raise ValueError('native account catalog is invalid or too large')
    accounts=[];agents=set()
    for row in raw['profiles']:
        if not isinstance(row,dict) or not isinstance(row.get('provider'),str) or row.get('provider') not in AGENTS: continue
        account={key:bounded(row.get(key),160 if key=='label' else 80) for key in ('id','provider','label')}
        for key in ('native','is_default','installed'):
            account[key] = row.get(key) is True
        cached = row.get('account_status') if isinstance(row.get('account_status'),dict) else {}
        status = cached.get('auth_status')
        if status not in ('signed_in','signed_out'):
            status = cached.get('status')
        account['auth_status'] = status if status in ('signed_in','signed_out','expired','credentials_locked','desktop_session_unavailable','credentials_unavailable') else 'unknown'
        if row['provider'] == 'opencode':
            account['model_options'] = model_options(row)
        accounts.append(account)
        if row.get('installed') is True: agents.add(row['provider'])
    result = {'accounts':accounts,'agents':sorted(agents)}
    if len(json.dumps(result,ensure_ascii=True).encode()) > MAX_CATALOG_BYTES:
        raise ValueError('native launch account projection exceeds size limit')
    return result


def projects(raw):
    value = normalize(raw, {'sessions': []}, 'launch')
    if not value['available'] or len(value['projects']) > 500:
        raise ValueError('native project catalog is unavailable')
    return {'project_launch_supported': True, 'swarm_id': value['swarm_id'], 'default_project': value['default_project'],
            'projects': [{key: row[key] for key in ('id','name','color','folders','accessible')} for row in value['projects'] if row['accessible']]}


def project_choice(catalog, payload, directory, worktree_catalog=None):
    if catalog.get('swarm_id') != payload['swarm_id']:
        raise ValueError('project catalog identity changed')
    project = next((row for row in catalog['projects'] if row['id'] == payload['project_id']), None)
    if project is None:
        raise ValueError('selected native project is unavailable')
    folder_id = payload.get('project_folder_id')
    if folder_id is not None:
        if not any(row['id'] == folder_id and row['path'] == directory for row in project['folders']):
            raise ValueError('selected local project folder changed')
    elif not payload['add_folder']:
        anchor = next((row for row in project['folders'] if row['id'] == payload.get('worktree_folder_id')), None)
        if (anchor is None or not isinstance(worktree_catalog,dict) or worktree_catalog.get('common_dir') != payload.get('worktree_common_dir')
                or not worktrees.contains(worktree_catalog,directory) or not worktrees.contains(worktree_catalog,anchor['path'])):
            raise ValueError('choose a current project folder, a verified related worktree, or explicitly add the folder')
    return project
