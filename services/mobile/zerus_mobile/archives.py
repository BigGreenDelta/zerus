"""Bounded read-only archive pages. JSON records and native identity stay authoritative."""
import json
import math
import re
import uuid

HELP_MARKER = 'archives --json'
MAX_DATE = 253402300800

def validate(payload):
    if not isinstance(payload, dict) or set(payload) - {'from','until','limit','cursor','query'}:
        raise ValueError('invalid archive query fields')
    start, end, limit = payload.get('from',0), payload.get('until',MAX_DATE), payload.get('limit',50)
    if type(start) is not int or type(end) is not int or not 0 <= start < end <= MAX_DATE or type(limit) is not int or not 1 <= limit <= 50:
        raise ValueError('invalid archive date range or page size')
    cursor, query = payload.get('cursor',''), payload.get('query','')
    if not isinstance(cursor,str) or len(cursor)>1024 or not re.fullmatch(r'[A-Za-z0-9_-]*',cursor):
        raise ValueError('invalid archive cursor')
    if not isinstance(query,str) or len(query)>256 or any(ord(c)<32 or 127<=ord(c)<=159 for c in query):
        raise ValueError('invalid archive search')

def page(value, payload):
    validate(payload)
    if (not isinstance(value,dict) or type(value.get('version')) is not int or value.get('version') != 1 or not isinstance(value.get('sessions'),list)
            or len(value['sessions'])>payload.get('limit',50) or type(value.get('total')) is not int
            or value['total']<len(value['sessions']) or not isinstance(value.get('next_cursor'),str)
            or len(value['next_cursor'])>1024 or not re.fullmatch(r'[A-Za-z0-9_-]*',value['next_cursor'])
            or value.get('from')!=payload.get('from',0) or value.get('until')!=payload.get('until',MAX_DATE)
            or value.get('query')!=payload.get('query','') or not value['sessions'] and value['next_cursor']):
        raise ValueError('invalid native archive page')
    seen=set(); previous=None
    for row in value['sessions']:
        if not isinstance(row,dict) or row.get('state')!='archived': raise ValueError('invalid archive row')
        archive=row.get('archive_id'); name=row.get('name'); at=row.get('archived_at')
        if (not isinstance(archive,str) or str(uuid.UUID(archive))!=archive or archive in seen or not isinstance(name,str)
                or not name or len(name)>512 or any(ord(c)<32 or ord(c)==127 for c in name)
                or type(at) not in (int,float) or not math.isfinite(at)
                or not payload.get('from',0)<=at<payload.get('until',MAX_DATE)):
            raise ValueError('invalid archive identity or date')
        order=(at,archive)
        if previous is not None and order>=previous: raise ValueError('archive page is out of order')
        previous=order;seen.add(archive)
    return value


PREVIEW_BYTES = 128 * 1024

def preview(value):
    """A bounded recent window for older phones that only understand snapshots."""
    page(value,{})
    fields={'name','cmd','agent','project','tag','run_id','conversation_id','archive_id','archived_at','created',
            'state','tracked','resumable','cwd','cwd_canonical','git_root','git_branch','git_worktree','model',
            'phase','activity','activity_summary','completion_reason','allowed_actions','action_reasons','account_id'}
    rows=[];size=2
    for raw in value['sessions']:
        row={k:v for k,v in raw.items() if k in fields}
        for key in ('prompt','last_message','title'):
            if isinstance(raw.get(key),str):row[key]=raw[key][:240]
        encoded=len(json.dumps(row,separators=(',',':')).encode())+1
        if size+encoded>PREVIEW_BYTES:break
        rows.append(row);size+=encoded
    return rows,dict(schema=1,available=True,total=value['total'],loaded=len(rows),partial=len(rows)<value['total'])
