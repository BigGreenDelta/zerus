#pragma once
// A fake `hgs task …` over a JSON file beside the script (tasks.json): enough of
// the real command's replies for the Tasks tab. Every call is appended to
// calls.jsonl as {"argv", "stdin"}. A file named "fail" holding a subcommand
// (list, add, send, …), optionally followed by flags it must carry ("settings
// --set"), makes those calls fail with "hgs refused: <subcommand>".
#include "FakeCli.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace FakeTasksCli {
inline const char *script() {
    return R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
here=pathlib.Path(__file__).resolve().parent
a=sys.argv[1:]
text=sys.stdin.read()
with open(here/'calls.jsonl','a') as f: f.write(json.dumps({'argv':a,'stdin':text})+'\n')
fail=here/'fail'
if a[:1]!=['task']: print('unexpected command',file=sys.stderr); sys.exit(2)
want=fail.read_text().split() if fail.exists() else []
if want and a[1]==want[0] and all(w in a for w in want[1:]): print('hgs refused: '+a[1],file=sys.stderr); sys.exit(1)
state=here/'tasks.json'
data=json.loads(state.read_text()) if state.exists() else {}
data.setdefault('tasks',[]); data.setdefault('next',1)
data.setdefault('settings',{'task_template':'[Zerus task {ref}]\n{prompt}','cancel_template':'[Zerus task {ref} cancelled]','agents_other':True,'agent_limit':0})
project={'id':'project-1','name':'orchard'}
now=int(time.time()*1000)
def opt(k): return a[a.index(k)+1] if k in a else None
def find(u): return next(t for t in data['tasks'] if t['uuid']==u or t['id']==u)
def save(): state.write_text(json.dumps(data))
def out(o): print(json.dumps(o))
def log(t,kind,body=''): t.setdefault('log',[]).append({'kind':kind,'actor':{'kind':'person'},'at':now,'text':body})
def top(t): data['tasks'].remove(t); data['tasks'].insert(0,t)
order=['review','active','todo','backlog','done']
cmd=a[1]
if cmd=='list':
    tasks=sorted(data['tasks'],key=lambda t: order.index(t['stage']))
    out({'project':project,'tasks':[{k:v for k,v in t.items() if k!='log'} for t in tasks],'damaged':data.get('damaged',[])}); sys.exit(0)
if cmd=='settings':
    if '--set' in a: data['settings'].update(json.loads(text)); save()
    out({'project':project,'settings':data['settings']}); sys.exit(0)
if cmd=='add':
    n=data['next']; data['next']=n+1
    target={'kind':'session','machine':'m-'+(opt('--host') or 'here'),'session':opt('--target'),'host':opt('--host') or ''} if opt('--target') else {'kind':'any'}
    t={'uuid':'uuid-%d'%n,'id':'T-%06d'%n,'ref':'orchard/T-%06d'%n,'text':text,'stage':'backlog' if '--backlog' in a else 'todo',
       'summary':'','cancel_reason':'','cancelled_at':None,'created_at':now,'stage_at':now,'writable':True,
       'author':{'kind':'person'},'target':target,'session':None,'rank':'V'}
    log(t,'created'); data['tasks'].append(t); save(); out({'task':t}); sys.exit(0)
t=find(a[2])
if cmd=='get': out({'project':project,'task':t}); sys.exit(0)
if cmd=='stage':
    t['stage']=a[3]; t['stage_at']=now; log(t,'stage',a[3])
    if a[3] in ('todo','backlog'): top(t)
elif cmd=='move':
    anchor=opt('--before') or opt('--after')
    if not anchor: top(t)
    else:
        data['tasks'].remove(t); i=data['tasks'].index(find(anchor)); data['tasks'].insert(i if '--before' in a else i+1,t)
elif cmd=='send':
    session=opt('--session') or t['target']['session']
    t['stage']='active'; t['stage_at']=now; t['session']={'machine':'m-here','session':session,'host':''}; log(t,'sent')
elif cmd=='cancel':
    t['stage']='review'; t['stage_at']=now; t['cancelled_at']=now; t['cancel_reason']=opt('--reason') or ''; log(t,'cancelled')
elif cmd=='edit': t['text']=text; log(t,'edited')
elif cmd=='target':
    t['target']={'kind':'any'} if a[3]=='any' else {'kind':'session','machine':'m-'+(opt('--host') or 'here'),'session':a[3],'host':opt('--host') or ''}
    log(t,'target',a[3])
elif cmd=='comment': log(t,'comment',text)
else: print('unknown task command '+cmd,file=sys.stderr); sys.exit(2)
save(); out({'task':t})
)PY";
}

// Installs the fake as dir/hgs, with `tasks` as its starting store and `damaged`
// task files it lists as unreadable.
inline bool install(const QDir &dir, const QJsonArray &tasks = {}, int damaged = 0) {
    QFile state(dir.filePath("tasks.json"));
    if (!state.open(QIODevice::WriteOnly)) return false;
    QJsonArray unreadable;
    for (int i = 0; i < damaged; ++i) unreadable << QJsonObject{{"uuid", QString("damaged-%1").arg(i)}, {"error", "not loro"}};
    state.write(QJsonDocument(QJsonObject{{"tasks", tasks}, {"next", int(tasks.size()) + 1}, {"damaged", unreadable}}).toJson(QJsonDocument::Compact));
    state.close();
    return FakeCli::install(dir.filePath("hgs"), script());
}

// A task view as `hgs task list` prints it. Extra fields replace the defaults.
inline QJsonObject task(int n, const QString &stage, const QJsonObject &extra = {}) {
    const QString id = QString("T-%1").arg(n, 6, 10, QLatin1Char('0'));
    QJsonObject t{{"uuid", QString("uuid-%1").arg(n)}, {"id", id}, {"ref", "orchard/" + id}, {"text", QString("Task %1").arg(n)},
                  {"stage", stage}, {"summary", ""}, {"cancel_reason", ""}, {"cancelled_at", QJsonValue()},
                  {"created_at", 1}, {"stage_at", 1}, {"writable", true}, {"author", QJsonObject{{"kind", "person"}}},
                  {"target", QJsonObject{{"kind", "any"}}}, {"session", QJsonValue()}, {"rank", "V"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) t[it.key()] = it.value();
    return t;
}

inline QJsonObject pinned(const QString &session, const QString &host = {}) {
    return {{"kind", "session"}, {"machine", "m-" + (host.isEmpty() ? QString("here") : host)}, {"session", session}, {"host", host}};
}

// The calls so far, as their argv lists.
inline QList<QStringList> calls(const QDir &dir) {
    QFile file(dir.filePath("calls.jsonl")); QList<QStringList> out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    while (!file.atEnd()) {
        QStringList argv;
        for (const auto &value : QJsonDocument::fromJson(file.readLine()).object().value("argv").toArray()) argv << value.toString();
        out << argv;
    }
    return out;
}

inline QStringList stdins(const QDir &dir) {
    QFile file(dir.filePath("calls.jsonl")); QStringList out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    while (!file.atEnd()) out << QJsonDocument::fromJson(file.readLine()).object().value("stdin").toString();
    return out;
}
}
