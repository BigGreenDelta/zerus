// HGS adapter for the official @deepseek-ai/dsh 0.2.0-rc.2 host.
// Loaded through its supported Cordis --patch extension mechanism. No fork of
// the engine, credential copying, HTTP authentication bypass or PTY injection.
import net from 'node:net';
import fs from 'node:fs/promises';
import { randomUUID } from 'node:crypto';
import { AsyncLocalStorage } from 'node:async_hooks';

export const name = 'hgs-native-dsh';
export const inject = ['sessionController', 'agents', 'goals', 'llm', 'agentDefaultModel', 'sessionQuery', 'userQuestions', 'workspaceRegistry', 'subagents', 'permissionPresets', 'approval', 'commands'];

export function apply(ctx, config) {
  const api = ctx.sessionController;
  // These extension seams are deliberately pinned to the tested release.
  // In particular the public selectModel also changes the profile default, and
  // only the private selection map tells whether an Agent has a selection.
  const control = api.agents;
  if (typeof control?.composeAgent !== 'function' || typeof control?.serializeImageAdmission !== 'function'
      || typeof control?.selectForNextRequest !== 'function' || !(control.selections instanceof WeakMap))
    throw new Error('HGS: incompatible official dsh session controller');
  const owned = new Map(), opening = new Map(), questions = new Map();
  const generation = randomUUID();
  const shellContext = new AsyncLocalStorage(), shellCalls = new Map(), jobCalls = new Map();
  const jobRegistry = ctx.get('jobs');
  if (jobRegistry) ctx.on('dispose',jobRegistry.events.subscribe({owners:'scope'},event => {
    const call = shellContext.getStore();
    if (event.type === 'registered' && event.job.kind === 'bash' && call?.owner === event.job.owner) {
      call.jobId=event.job.id;jobCalls.set(event.job.id,call);
    }
    if (event.type === 'removed') jobCalls.delete(event.job.id);
  }));
  ctx.on('tools/execute',async (exec,next) => {
    if (exec.name !== 'bash' || !exec.agent?.session?.id) return next();
    const call={callId:exec.callId,owner:exec.agent.session.id,command:String(exec.arguments?.command ?? '').slice(0,16000),
      cwd:exec.arguments?.workdir?.slice(0,4096),startedAt:Date.now(),status:'running'};
    const key=JSON.stringify([call.owner,call.callId]);shellCalls.set(key,call);
    while(shellCalls.size>512)shellCalls.delete(shellCalls.keys().next().value);
    return shellContext.run(call,async()=>{
      try {
        const result=await next();
        call.status=result.isError?'failed':result.value?.kind==='background'||result.value?.kind==='promoted'?'running':'completed';
        if(call.status!=='running')call.finishedAt=Date.now();
        call.exitCode=result.value?.exitCode;
        return result;
      } catch(error){call.status='failed';call.finishedAt=Date.now();throw error;}
    });
  });
  const recovery = new Map(), recoveryReceipts = new Map();
  const compactions = new Map(), compactReceipts = new Set();
  const compactionPublic = sessionId => compactions.get(sessionId)?.public ?? null;
  const recoveryPublic = sessionId => recovery.get(sessionId)?.public ?? null;
  async function recoveryEnabled() {
    if (!config.recoveryPolicy) return false;
    try {
      const policy = JSON.parse(await fs.readFile(config.recoveryPolicy, 'utf8'));
      const heartbeat = JSON.parse(await fs.readFile(config.recoveryPolicy.replace(/policy\.json$/, 'heartbeat.json'), 'utf8'));
      return policy.enabled === true && heartbeat.enabled === true && Date.now() / 1000 - heartbeat.at < 30;
    } catch { return false; }
  }
  // Native retry policy goes first. HGS only owns the final failed model step.
  ctx.on('agent/request-error', async (request, next) => {
    const native = await next();
    const sessionId = request.agent?.session?.id;
    if (native || !owned.has(sessionId) || closing || !await recoveryEnabled()) return native;
    return new Promise(resolve => {
      const id = randomUUID();
      const finish = result => {
        if (recovery.get(sessionId)?.public.id === id) recovery.delete(sessionId);
        clearInterval(timer); request.signal?.removeEventListener('abort', abort); resolve(result);
      };
      const abort = () => finish(undefined);
      const timer = setInterval(async () => { if (!await recoveryEnabled()) finish(undefined); }, 3000);
      timer.unref();
      const failure = request.failure;
      const retryAfter = Number(failure?.providerRetryAfterMs ?? 0);
      recovery.set(sessionId, { finish, public: { id, at: Date.now() / 1000,
        turn: request.turn, step: request.step,
        detail: `${failure?.status ?? ''} ${failure?.code ?? ''} ${failure?.message ?? 'Provider request failed'}`.trim(),
        retryNotBefore: Number.isFinite(retryAfter) && retryAfter > 0 ? (Date.now() + retryAfter) / 1000 : 0 } });
      request.signal?.addEventListener('abort', abort, { once: true });
      if (request.signal?.aborted) abort();
    });
  }, { prepend: true });
  let closing = false;
  // Read only the native account's public projection, never its credentials.
  const accountStatus = async () => (await ctx.get('deepseekAccount')?.getState())?.status ?? 'unavailable';
  // deepseek-official uses API keys; a signed-out account must not block it.
  const needsSignIn = (selection, status) => selection?.provider === 'deepseek-account' && status === 'signed-out';
  async function publishWorkspace(sessionId) {
    const observation = await ctx.sessionQuery.observeSession(sessionId);
    try {
      if (observation.header.origin === 'subagent') throw new Error('Use the parent workspace for a subagent');
      const workspace = await ctx.workspaceRegistry.create(observation.header.cwd);
      await workspace.attachSession(sessionId);
      return { workspaceId: workspace.id, path: workspace.path, sessionIds: [...workspace.sessionIds] };
    } finally { observation[Symbol.dispose](); }
  }
  function applyPermissions(agent, mode) {
    if (mode === 'bypass') ctx.permissionPresets.apply(agent.session, 'danger-full-access',
      policy => ctx.approval.setPolicy(agent, policy));
  }
  function checkPermissions(mode) {
    if (mode !== undefined && !['provider', 'bypass'].includes(mode)) throw new Error('Unknown account permission mode');
  }
  async function resume(sessionId, permissionMode) {
    checkPermissions(permissionMode);
    const live = ctx.agents.get(sessionId);
    if (live) return live;
    if (opening.has(sessionId)) return opening.get(sessionId);
    const work = (async () => {
      const observation = await ctx.sessionQuery.observeSession(sessionId);
      try {
        if (observation.header.origin === 'subagent') throw new Error('Use the parent to manage a subagent');
        const composition = await control.composeAgent(control.presetForObservation(observation));
        const { provider, model } = ctx.agentDefaultModel.currentSelection();
        const handle = await ctx.agents.resume({ resumeSessionId: sessionId, agentOptions: { provider, model }, setup: composition.setup });
        applyPermissions(handle.agent, permissionMode);
        owned.set(sessionId, handle);
        await publishWorkspace(sessionId);
        return handle.agent;
      } finally { observation[Symbol.dispose](); }
    })().finally(() => opening.delete(sessionId));
    opening.set(sessionId, work);
    return work;
  }
  // Upstream installs a model selection while composing a top-level Agent or
  // changing its model. selectionFor would also install one on a read of any
  // other Agent, such as a child: prompt listeners that take over its request
  // route (the profile default before its first request), and an effect Cordis
  // rejects on an unloading scope. Reads never install one; an Agent without
  // one reports its stored selection, or before its first request its own route.
  const selectionOf = (agent, projections) => {
    const installed = control.selections.get(agent);
    if (installed) return { ...installed.current };
    const stored = projections?.modelSelection?.next;
    if (stored) return stored;
    const { provider, model, reasoningEffort } = agent.options ?? {};
    return provider && model ? { provider, model, ...(reasoningEffort === undefined ? {} : { reasoningEffort }) } : null;
  };
  // A child lives in its parent's Agent scope and belongs to subagent routing:
  // upstream neither prompts nor reconfigures one directly, and HGS messages it
  // through send-child. Send and settings refuse any other than a top-level
  // Agent before they change it.
  const topLevel = agent => {
    if (agent && (agent.session.header.origin === 'subagent' || !ctx.agents.roots().includes(agent)))
      throw new Error('Use the parent to manage a subagent');
    return agent;
  };
  const publicQuestions = sessionId => [...questions.values()].filter(q => q.sessionId === sessionId)
    .map(({ id, items, approval }) => ({ id, questions: items, approval: Boolean(approval) }));
  // A live request is the only authority for answering. Disconnecting HGS does
  // not answer, dismiss, or approve it. Native cancellation releases the wait.
  ctx.on('user-questions/request', (request, next) => {
    const sessionId = request.agent?.session?.id;
    if (!owned.has(sessionId) || closing) return next();
    return new Promise((resolve, reject) => {
      const id = request.wait?.callId ?? randomUUID();
      const abort = () => { questions.delete(id); reject(request.signal.reason ?? new Error('Question cancelled')); };
      if (request.signal?.aborted) { abort(); return; }
      const settle = answer => { questions.delete(id); request.signal?.removeEventListener('abort', abort); resolve(answer); };
      questions.set(id, { id, sessionId, items: request.questions, settle });
      request.signal?.addEventListener('abort', abort, { once: true });
      // Timed questions must claim the native wait, not invent a separate timer.
      if (request.wait?.timed) {
        (async () => { for await (const _ of ctx.userQuestions.attachWait(request.agent, id, request.signal)) {} })()
          .catch(() => {});
      }
    });
  }, { prepend: true });
  ctx.on('approval/request', (request, next) => {
    const sessionId = request.agent?.session?.id;
    if (!owned.has(sessionId) || closing) return next();
    return new Promise(resolve => {
      const id = randomUUID();
      const abort = () => { questions.delete(id); resolve('cancelled'); };
      if (request.signal?.aborted) { abort(); return; }
      const settle = answer => {
        questions.delete(id); request.signal?.removeEventListener('abort', abort);
        resolve(answer.answers[0].selected[0] === 'Allow once' ? 'allowed-once' : 'rejected');
      };
      questions.set(id, { id, sessionId, approval: true, settle, items: [{ id: 'decision',
        header: 'Approval required', question: `Allow ${request.toolName}?`, detail: request.reason ?? request.displayReason?.en ?? '',
        options: [{ label: 'Allow once' }, { label: 'Reject' }] }] });
      request.signal?.addEventListener('abort', abort, { once: true });
    });
  }, { prepend: true });

  async function childPage(address) {
    const abort = new AbortController();
    const stream = api.follow({ address, maxMessages: 150 }, abort.signal);
    try {
      const first = await stream.next();
      if (first.value?.type !== 'snapshot') throw new Error('Native child history is unavailable');
      return { baseline: first.value.projections, page: { records: first.value.records, hasMore: first.value.hasMore } };
    } finally { abort.abort(); await stream.return(); }
  }
  // Native authorization reports a child without its own descriptor as corrupt.
  // A one-shot child records it in its first step, after its parent catalogs
  // it and after that step's durability checkpoint, so a new child looks the
  // same until then. Such a live child has no public history yet.
  async function childHistory(address) {
    try { return await childPage(address); } catch (error) {
      if (error?.code !== 'subagent/catalog-diagnostic') throw error;
      const state = await childEstablishment(address).catch(() => 'unknown');
      if (state === 'starting') return { baseline: { asOfSeq: -1, values: {} }, page: { records: [], hasMore: false }, starting: true };
      if (state !== 'recorded') throw error;
    }
    // A descriptor recorded since the first read passes native validation;
    // a corrupt one fails it again.
    return childPage(address);
  }
  async function childEstablishment({ parentSessionId, childSessionId }) {
    const observation = await ctx.sessionQuery.observeSession(childSessionId, { projectionMode: 'none' });
    try {
      const { header, inheritedEventCount } = observation;
      if (header.origin !== 'subagent' || header.parentSession !== parentSessionId) return 'unrelated';
      const own = observation.events.slice(inheritedEventCount);
      if (own.some(event => event.type === 'subagent/descriptor')) return 'recorded';
      // Agent disposal drains storage before a session leaves the live
      // registry, so a stored child without a descriptor stays a diagnostic.
      return observation.source === 'live' && !own.some(event => event.type === 'step/start') ? 'starting' : 'missing';
    } finally { observation[Symbol.dispose](); }
  }
  async function inspect(request) {
    const { sessionId, agentId } = request;
    let baseline = await api.projections({ sessionId }, new AbortController().signal);
    if (!baseline) throw new Error('Native session no longer exists');
    const child = agentId ? baseline.values.subagentCatalog?.find(child => child.id === agentId) : null;
    const jobOwners = agentId ? [agentId] : [sessionId, ...(baseline.values.subagentCatalog ?? []).map(child => child.id)];
    let address = { kind: 'session', sessionId };
    let page, starting = false;
    if (agentId) {
        // Native page authorization verifies the direct parent/child relation.
        address = { kind: 'subagent', parentSessionId: sessionId, childSessionId: agentId, mode: 'unknown' };
        ({ baseline, page, starting = false } = await childHistory(address));
    }
    page ??= await api.page({ address, throughSeq: baseline.asOfSeq, maxMessages: 150 }, new AbortController().signal);
    const lifecycleRows = agentId ? null : await api.list({}, new AbortController().signal);
    const lifecycleIdle = lifecycleRows?.items?.find(row => row.sessionId === sessionId)?.running === false;
    const parentLive = ctx.agents.get(sessionId);
    const live = agentId ? ctx.agents.get(agentId) : parentLive;
    const selection = live ? selectionOf(live, baseline.values) : null;
    // Read the goal before awaiting: a disposing Agent may leave the live
    // registry meanwhile, and the goal registry rejects it once it has.
    const goal = live ? ctx.goals.get(live) ?? null : baseline.values.goal ?? null;
    const status = await accountStatus();
    const jobs = ctx.get('jobs');
    return { generation, baseline, page,
      lifecycleActions: agentId ? [] : ["rename", ...(!parentLive ? ["resume", "forget"] : owned.get(sessionId)?.agent === parentLive ? [...(lifecycleIdle ? ["pause"] : []), "forget"] : [])],
      jobs: jobs && request.includeProcesses !== false ? jobOwners.flatMap(owner => jobs.list(owner).filter(job => job.kind === 'bash' && job.owner === owner)
        .map(job => ({ ...job, callId:jobCalls.get(job.id)?.callId, controllable: owned.has(sessionId) && Boolean(ctx.agents.get(owner)) }))) : undefined,
      shellCalls:request.includeProcesses !== false ? [...shellCalls.values()].filter(call=>jobOwners.includes(call.owner)) : undefined,
      childMode: child?.mode, childSendSupported: !starting && child?.mode === 'continuable' && Boolean(parentLive), goal,
      pendingQuestions: publicQuestions(sessionId), selection, accountStatus: status, authRequired: needsSignIn(selection, status),
      live: Boolean(live), owned: owned.has(sessionId), nativeRecovery: recoveryPublic(sessionId), interruptSupported: typeof api.cancel === 'function',
      compactSupported: Boolean(live && ctx.commands.find(live,'compact')?.definitionId === '@deepseek-ai/dsh-command-compact'), compactRequest: agentId ? null : compactionPublic(sessionId) };
  }
  async function execute(request) {
    if (closing) throw new Error('Native host is stopping');
    const p = request.params ?? {};
    switch (request.method) {
      case 'ping': return { protocol: 1, version: '0.2.0-rc.2', generation, accountPermissions: true, sessionActions: true };
      case 'list': {
        const list = await api.list({}, new AbortController().signal);
        const status = await accountStatus();
        return { ...list, generation, items: list.items.map(row => {
          const agent = ctx.agents.get(row.sessionId);
          const selection = agent ? selectionOf(agent, row.projections?.values) : null;
          return { ...row, selection, accountStatus: status, authRequired: needsSignIn(selection, status),
            goal: agent ? ctx.goals.get(agent) ?? null : row.projections?.values.goal ?? null,
            pendingQuestions: publicQuestions(row.sessionId), nativeRecovery: recoveryPublic(row.sessionId) };
        }) };
      }
      case 'inspect': return inspect(p);
      case 'job-output':
      case 'job-stop': {
        if (p.generation !== generation) throw new Error('DeepSeek host changed; refresh Processes');
        const fresh = await inspect({sessionId:p.sessionId});
        const job = fresh.jobs?.find(job => job.id === p.jobId && job.owner === p.owner);
        if (!job) throw new Error('This process no longer belongs to the selected session');
        const jobs = ctx.get('jobs');
        if (request.method === 'job-stop') {
          if (!job.controllable) throw new Error('Native process control is unavailable');
          return {status:jobs.kill(job.id,job.owner,'Stopped from Zerus Processes'),generation};
        }
        const from = Math.max(0,job.output.total-32768);
        const read = jobs.readAt(job.id,from,job.owner);
        const bytes = Buffer.from(read.chunks.map(chunk => chunk.text).join(''),'utf8');
        return {output:bytes.subarray(Math.max(0,bytes.length-32768)).toString('utf8'),
          truncated:from>0 || read.lossy || bytes.length>32768, status:job.status, generation};
      }
      case 'compact': {
        if (p.generation !== generation) throw new Error('DeepSeek host changed');
        const agent = ctx.agents.get(p.sessionId);
        if (!agent || ctx.commands.find(agent,'compact')?.definitionId !== '@deepseek-ai/dsh-command-compact') throw new Error('Native compaction is unavailable');
        if (compactReceipts.has(p.requestId) || compactionPublic(p.sessionId)?.status === 'compacting') throw new Error('Compaction was already requested');
        const abort = new AbortController();
        const operation = ctx.commands.execute(agent, '/compact', [], abort.signal);
        const state = { public: { request_id: p.requestId, status: 'compacting', at: Date.now() / 1000 }, abort };
        compactions.set(p.sessionId, state); compactReceipts.add(p.requestId);
        operation.then(async result => {
          if (state.public.status !== 'compacting') return;
          state.revision = (await api.projections({sessionId:p.sessionId}, new AbortController().signal)).asOfSeq;
          state.public.status = result?.result.kind !== 'success' ? 'failed' : result.result.sourceEventSeq !== undefined ? 'completed' : 'unchanged';
          state.public.error = result?.result.kind === 'error' ? result.result.text : undefined;
          state.public.completed_at = Date.now() / 1000;
        }).catch(error => { state.public.status = 'failed'; state.public.error = String(error?.message ?? error); });
        return { status: 'submitted' };
      }
      case 'interrupt': {
        if (p.generation !== generation) throw new Error('DeepSeek host changed');
        const fresh = await inspect({ sessionId: p.sessionId });
        const started = fresh.page.records.findLast(row => row.event?.type === 'turn/start')?.event.time / 1000;
        const live = ctx.agents.get(p.sessionId);
        if (!live || !(started > 0) || started !== p.turnStarted) throw new Error('DeepSeek turn changed; refresh Activity');
        ctx.goals.disarm(live);
        const compaction = compactions.get(p.sessionId);
        if (compaction?.public.status === 'compacting') {
          compaction.public.status = 'cancelled'; compaction.abort.abort();
        }
        recovery.get(p.sessionId)?.finish(undefined);
        return api.cancel({ sessionId: p.sessionId });
      }
      case 'cancel-recovery': {
        const pending = recovery.get(p.sessionId);
        if (pending?.public.id === p.id) pending.finish(undefined);
        return { cancelled: true };
      }
      case 'recover': {
        if (p.generation !== generation) throw new Error('DeepSeek host changed');
        const previous = recoveryReceipts.get(p.requestId);
        if (previous) {
          if (previous.sessionId !== p.sessionId || previous.id !== p.id) throw new Error('Recovery request ID reused');
          return previous;
        }
        const pending = recovery.get(p.sessionId);
        if (!pending || pending.public.id !== p.id || !await recoveryEnabled()) throw new Error('Failed request is no longer pending');
        if (Date.now() / 1000 < pending.public.retryNotBefore) throw new Error('Provider retry deadline has not passed');
        const receipt = { requestId: p.requestId, sessionId: p.sessionId, id: p.id, generation, status: 'confirmed' };
        recoveryReceipts.set(p.requestId, receipt);
        if (recoveryReceipts.size > 1000) recoveryReceipts.delete(recoveryReceipts.keys().next().value);
        pending.finish({ kind: 'retry' });
        return receipt;
      }
      case 'workspace': return publishWorkspace(p.sessionId);
      case 'catalog': return api.modelCatalog();
      case 'create': {
        checkPermissions(p.permissionMode);
        if (typeof p.cwd !== 'string' || !p.cwd.startsWith('/') || !(await fs.stat(p.cwd)).isDirectory()) throw new Error('Choose an existing absolute folder');
        const sessionId = p.sessionId;
        if (typeof sessionId !== 'string' || !/^session-[0-9a-f-]{36}$/.test(sessionId)) throw new Error('Invalid native session ID');
        if (ctx.agents.get(sessionId)) {
          const observation=await ctx.sessionQuery.observeSession(sessionId);
          try {if (observation.header.cwd !== p.cwd) throw new Error('Native session belongs to another folder');}
          finally {observation[Symbol.dispose]();}
          await publishWorkspace(sessionId);
          return {sessionId,generation};
        }
        try {
          const observation = await ctx.sessionQuery.observeSession(sessionId);
          try {
            if (observation.header.cwd !== p.cwd) throw new Error('Native session belongs to another folder');
          } finally { observation[Symbol.dispose](); }
          await resume(sessionId, p.permissionMode); return { sessionId, generation };
        } catch (error) {
          if (error?.code !== 'SESSION_QUERY_SESSION_NOT_FOUND') throw error;
        }
        const composition = await control.composeAgent('standard');
        const { provider, model } = ctx.agentDefaultModel.currentSelection();
        const handle = await ctx.agents.create({ sessionId, agentOptions: { provider, model },
          meta: { cwd: p.cwd, agentPreset: composition.agentPreset }, setup: composition.setup });
        applyPermissions(handle.agent, p.permissionMode);
        owned.set(sessionId, handle);
        await publishWorkspace(sessionId);
        if (p.title) await api.rename({ sessionId, title: p.title });
        return { sessionId, generation };
      }
      case 'session-action': {
        let changed = false;
        const answer = (status, error) => ({ sessionId:p.sessionId, action:p.action, generation, status, ...(error ? {error} : {}) });
        try {
          const listed = p.action === 'pause' ? await api.list({},new AbortController().signal) : null;
          if (p.generation !== generation) throw new Error('DeepSeek host changed; refresh before this action');
          if (!['pause','resume','rename','forget'].includes(p.action) || typeof p.sessionId !== 'string') throw new Error('Unsupported scoped lifecycle action');
          const live = ctx.agents.get(p.sessionId);
          const handle = owned.get(p.sessionId);
          if (p.action === 'pause' && listed?.items?.find(row => row.sessionId === p.sessionId)?.running === true) throw new Error('Wait for the native turn before pausing');
          if (live && p.action !== 'rename' && handle?.agent !== live) throw new Error('This agent is owned by the native UI; manage it there');
          if (p.action === 'resume') {
            if (live || opening.has(p.sessionId)) throw new Error('This session is already running or opening');
            checkPermissions(p.permissionMode);
            changed = true;
            await resume(p.sessionId,p.permissionMode);
          } else if (p.action === 'rename') {
            if (typeof p.title !== 'string' || !p.title || p.title.length > 512) throw new Error('Invalid scoped title');
            changed = true; await api.rename({sessionId:p.sessionId,title:p.title});
          } else if (live) {
            changed = true; ctx.goals.disarm(live);
            await handle.dispose();
            if (owned.get(p.sessionId) === handle) owned.delete(p.sessionId);
          }
          return answer('completed');
        } catch (error) { return answer(changed ? 'uncertain' : 'failed',String(error?.message ?? error)); }
      }
      case 'resume': await resume(p.sessionId, p.permissionMode); return { generation };
      case 'pause': {
        // Closing the owned handle drains exactly this agent and its children;
        // other sessions and the shared native host remain alive.
        const live = ctx.agents.get(p.sessionId);
        if (!live) return { stopped: true };
        const handle = owned.get(p.sessionId);
        if (!handle) throw new Error('This agent is owned by the native UI; stop it there');
        ctx.goals.disarm(live);
        await handle.dispose(); owned.delete(p.sessionId);
        return { stopped: true };
      }
      case 'rename': return api.rename(p);
      case 'send-child': {
        const parent = ctx.agents.get(p.parentSessionId);
        if (!parent) throw new Error('Resume the parent session before messaging this agent');
        return ctx.subagents.prompt(p, new AbortController().signal);
      }
      case 'send': {
        topLevel(ctx.agents.get(p.sessionId));
        const compact = compactionPublic(p.sessionId);
        if (p.expectedCompactionId && (compact?.request_id !== p.expectedCompactionId || compact.status !== 'completed'))
          throw new Error('Compaction is not confirmed; message was not sent');
        if (p.expectedCompactionId && compactions.get(p.sessionId).revision !== (await api.projections({sessionId:p.sessionId}, new AbortController().signal)).asOfSeq)
          throw new Error('Conversation changed after compaction; message was not sent');
        // A message consumes the continuation even if another client sent it.
        if (compact) compact.status = 'cancelled';
        recovery.get(p.sessionId)?.finish(undefined);
        const agent = ctx.agents.get(p.sessionId);
        if (!agent) throw new Error('Resume this session before sending a message');
        // Read as list and inspect do. A top-level Agent upstream composed has an
        // installed selection; another one, such as a webhook root, gets none here.
        const selection = selectionOf(agent, control.selections.has(agent) ? undefined
          : (await api.projections({ sessionId: p.sessionId }, new AbortController().signal))?.values);
        if (needsSignIn(selection, await accountStatus()))
          throw new Error('Sign in to DeepSeek in the native UI before sending a message');
        return api.prompt(p, new AbortController().signal);
      }
      case 'settings': {
        const agent = topLevel(await resume(p.sessionId));
        const catalog = await api.modelCatalog();
        const model = catalog.groups.find(g => g.id === p.provider)?.models.find(m => m.id === p.model);
        if (!model) throw new Error('This model is unavailable for the native account');
        if (p.reasoningEffort !== undefined && !model.reasoning?.efforts?.some(e => e.id === p.reasoningEffort)) throw new Error('Unsupported reasoning effort');
        return control.serializeImageAdmission(agent, async () => {
          const resolved = await ctx.llm.resolveCallConfig({ provider: p.provider, model: p.model,
            ...(p.reasoningEffort === undefined ? {} : { reasoningEffort: p.reasoningEffort }) });
          const selection = { provider: resolved.provider, model: resolved.model,
            ...(resolved.reasoningEffort === undefined ? {} : { reasoningEffort: resolved.reasoningEffort }) };
          control.selectForNextRequest(agent, selection);
          return { selected: selection, scope: 'session' };
        });
      }
      case 'answer': {
        const q = questions.get(p.id);
        if (p.generation !== generation || !q || q.sessionId !== p.sessionId) throw new Error('This question is no longer pending');
        if (!Array.isArray(p.answers) || p.answers.length !== q.items.length) throw new Error('Answer every question');
        const seen = new Set();
        for (const answer of p.answers) {
          const item = q.items.find(i => i.id === answer.id);
          if (!item || seen.has(answer.id) || !Array.isArray(answer.selected)
              || answer.selected.some(label => !item.options?.some(o => o.label === label))
              || (!item.multiSelect && answer.selected.length > 1)
              || (answer.selected.length === 0 && !answer.custom?.trim())
              || (q.approval && answer.selected.length !== 1)
              || (answer.custom !== undefined && typeof answer.custom !== 'string')) throw new Error('Invalid question answer');
          seen.add(answer.id);
        }
        q.settle({ answers: p.answers }); return { accepted: true };
      }
      default: throw new Error('Unsupported HGS native request');
    }
  }

  // A private Unix socket is a local plugin transport, never an HTTP endpoint.
  // The native web application's own authentication remains untouched.
  const server = net.createServer(socket => {
    let bytes = 0, chunks = [], handled = false;
    socket.setTimeout(45000, () => socket.destroy());
    socket.on('error', () => {});
    socket.on('data', chunk => {
      if (handled) return;
      bytes += chunk.length;
      if (bytes > 30 * 1024 * 1024) { socket.destroy(); return; }
      chunks.push(chunk);
      if (!chunk.includes(10)) return;
      handled = true;
      (async () => {
        try {
          const request = JSON.parse(Buffer.concat(chunks).toString('utf8'));
          socket.end(JSON.stringify({ ok: true, value: await execute(request) }) + '\n');
        } catch (error) {
          socket.end(JSON.stringify({ ok: false, error: String(error?.message ?? error), code: error?.code }) + '\n');
        }
      })();
    });
  });
  ctx.effect(async () => {
    await new Promise((resolve, reject) => {
      server.once('error', reject); server.listen(config.socket, resolve);
    });
    await fs.chmod(config.socket, 0o600);
    return async () => {
      closing = true; server.close();
      for (const handle of owned.values()) await handle.dispose();
      owned.clear(); await fs.unlink(config.socket).catch(() => {});
    };
  }, 'hgs.native-dsh');
}
