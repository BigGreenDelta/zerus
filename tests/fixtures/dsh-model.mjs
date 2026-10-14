// Deterministic, keyless model running inside the official release for adapter
// integration tests. This fixture is never loaded by the installed HGS host.
import { existsSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
const require = createRequire(process.env.HGS_DSH_TEST_PACKAGE + '/package.json');
const { LlmAdapter } = await import(pathToFileURL(require.resolve('@deepseek-ai/dsh-llm')));
const { defineTool } = await import(pathToFileURL(require.resolve('@deepseek-ai/dsh-tools')));
class Adapter extends LlmAdapter {
  failedRecovery = new Set();
  providerInfo(provider) { return { id: provider, name: 'HGS test model' }; }
  async listModels(provider) { return [{ provider, id: 'test-a', name: 'Test A' }, { provider, id: 'test-b', name: 'Test B' }]; }
  async resolveModel(provider,model) { return { provider,id:model,name:model,
    reasoning:{efforts:[{id:'off',name:'Off'},{id:'high',name:'High'}],defaultEffort:'high'} }; }
  async *stream(options) {
    await new Promise(resolve => setTimeout(resolve, 120));
    const prompt = options.messages.filter(m => m.role === 'user' && m.source?.kind === 'user').at(-1);
    const text = prompt?.content?.filter(b => b.type === 'text').map(b => b.text).join('') ?? '';
    if (text.includes('INTERRUPT_WAIT_FIXTURE')) {
      await new Promise((resolve,reject) => {
        const timer=setTimeout(resolve,10000);
        const abort=()=>{clearTimeout(timer);reject(new DOMException('Interrupted','AbortError'));};
        if(options.signal?.aborted)abort();else options.signal?.addEventListener('abort',abort,{once:true});
      });
    }
    if (text.includes('PROVIDER_RECOVER_ONCE_FIXTURE') && !this.failedRecovery.has(text)) {
      this.failedRecovery.add(text);
      throw Object.assign(new Error('503 Service unavailable'), {code:'service_unavailable'});
    }
    if (text.includes('PROVIDER_CAPACITY_FIXTURE')) throw Object.assign(new Error('Selected model is at capacity. Please try a different model.'), {code:'model_capacity'});
    const last = options.messages.at(-1);
    if ((text.includes('SHELL_BACKGROUND_FIXTURE') || text.includes('SHELL_FOREGROUND_FIXTURE')) && last?.role !== 'tool') {
      const background=text.includes('SHELL_BACKGROUND_FIXTURE');
      const args=JSON.stringify({command:background?"printf 'independent-output\\n'; sleep 30":"printf 'foreground-output\\n'; exit 7",description:'Shell process test',run_in_background:background});
      yield {type:'block-start',index:0,blockType:'tool-call'};
      yield {type:'tool-call-delta',index:0,id:'shell-call',name:'bash',argumentsDelta:args};
      yield {type:'block-end',index:0,block:{type:'tool-call',id:'shell-call',name:'bash',arguments:args}};
      yield {type:'finish',reason:{kind:'tool-calls'}};return;
    }
    if (text.includes('SHELL_READ_FIXTURE') && last?.role !== 'tool') {
      yield {type:'block-start',index:0,blockType:'tool-call'};
      yield {type:'tool-call-delta',index:0,id:'read-call',name:'hgs_test_job_read',argumentsDelta:'{}'};
      yield {type:'block-end',index:0,block:{type:'tool-call',id:'read-call',name:'hgs_test_job_read',arguments:'{}'}};
      yield {type:'finish',reason:{kind:'tool-calls'}};return;
    }
    if (text.includes('APPROVAL') && last?.role !== 'tool') {
      yield {type:'block-start',index:0,blockType:'tool-call'};
      yield {type:'tool-call-delta',index:0,id:'approval-call',name:'hgs_test_approval',argumentsDelta:'{}'};
      yield {type:'block-end',index:0,block:{type:'tool-call',id:'approval-call',name:'hgs_test_approval',arguments:'{}'}};
      yield {type:'finish',reason:{kind:'tool-calls'}};return;
    }
    if (text.includes('SUBAGENT') && last?.role !== 'tool') {
      const args=JSON.stringify({description:'Native child fixture',prompt:'Child result fixture'+(text.match(/ (?:HOLD|DISPOSING)_CHILD_\w+/)?.[0]??''),run_in_background:text.includes('CONTINUABLE')});
      yield {type:'block-start',index:0,blockType:'tool-call'};
      yield {type:'tool-call-delta',index:0,id:'child-call',name:'subagent',argumentsDelta:args};
      yield {type:'block-end',index:0,block:{type:'tool-call',id:'child-call',name:'subagent',arguments:args}};
      yield {type:'finish',reason:{kind:'tool-calls'}};return;
    }
    if (text.includes('QUESTION') && last?.role !== 'tool') {
      const args=JSON.stringify({ questions:[{id:'choice',question:'Which option?',options:[{label:'First'},{label:'Second'}]}] });
      yield {type:'block-start',index:0,blockType:'tool-call'};
      yield {type:'tool-call-delta',index:0,id:'question-call',name:'ask_user_question',argumentsDelta:args};
      yield {type:'block-end',index:0,block:{type:'tool-call',id:'question-call',name:'ask_user_question',arguments:args}};
      yield {type:'finish',reason:{kind:'tool-calls'}};return;
    }
    const reply=text.includes('ATTACHMENT_ORDER_FIXTURE')
      ? JSON.stringify(prompt.content.map(block => block.type === 'text' ? block.text : block.type))
      : 'Native reply: '+text;
    yield {type:'block-start',index:0,blockType:'reasoning'};
    yield {type:'reasoning-delta',index:0,text:'PRIVATE_TEST_REASONING'};
    yield {type:'block-end',index:0,block:{type:'reasoning',text:'PRIVATE_TEST_REASONING'}};
    yield {type:'block-start',index:1,blockType:'text'};
    yield {type:'text-delta',index:1,text:reply};
    yield {type:'block-end',index:1,block:{type:'text',text:reply}};
    yield {type:'usage',usage:{inputTokens:4,outputTokens:5}};
    yield {type:'finish',reason:{kind:'stop'}};
  }
}
export const inject=['llm','goals','tools','approval','jobs'];
export function apply(ctx,config) {
  ctx.llm.registerAdapter(['hgs-test'],new Adapter());
  // Hold a marked child's first step, before it records its subagent
  // descriptor, until the test creates the release file (as a slow checkpoint).
  ctx.on('agent/pre-step',async(request,next)=>{
    const hold=request.agent?.session.header.origin==='subagent'&&JSON.stringify(request.messages??[]).match(/HOLD_CHILD_\w+/)?.[0];
    for(let i=0;hold&&config?.holds&&i<300&&!existsSync(join(config.holds,hold));i++)await new Promise(resolve=>setTimeout(resolve,50));
    return next();
  });
  // Native disposal unloads an agent's Cordis scope before the agent leaves
  // the live registry. Keep a marked child in that window, as a slow scope
  // teardown would, and report it with a marker file until the test releases it.
  const disposing=new WeakSet();
  ctx.on('agent/pre-step',(request,next)=>{
    const agent=request.agent;
    const hold=agent?.session.header.origin==='subagent'&&JSON.stringify(request.messages??[]).match(/DISPOSING_CHILD_\w+/)?.[0];
    if(hold&&config?.holds&&!disposing.has(agent)){
      disposing.add(agent);
      agent.ctx.effect(()=>async()=>{
        writeFileSync(join(config.holds,hold+'.disposing'),'');
        for(let i=0;i<300&&!existsSync(join(config.holds,hold));i++)await new Promise(resolve=>setTimeout(resolve,50));
      },'hgs-test.disposal-hold');
    }
    return next();
  });
  ctx.tools.register(defineTool({name:'hgs_test_job_read',description:'Verify the model output cursor',parameters:{},
    output:{schema:{type:'string'},render:(_args,value)=>[{type:'text',text:value}]},
    async execute(_args,exec){const owner=exec.agent.session.id;const job=ctx.jobs.list(owner).find(j=>j.owner===owner&&j.kind==='bash');return ctx.jobs.read(job.id,owner).chunks.map(c=>c.text).join('');}
  }));
  ctx.tools.register(defineTool({name:'hgs_test_approval',description:'Keyless approval fixture',parameters:{},
    output:{schema:{type:'string'},render:(_args,value)=>[{type:'text',text:value}]},
    async execute(_args,exec){return ctx.approval.request({agent:exec.agent,toolName:'hgs_test_approval',callId:exec.callId,reason:'Test an explicit native decision',signal:exec.signal});}
  }));
  ctx.on('agent/created',({agent,source})=>{
    if (source !== 'startup') return;
    const created=ctx.goals.create(agent,{objective:'Test the native adapter',maxGoalRounds:8});
    ctx.goals.pause(agent,{id:created.id,revision:created.revision});
  });
}
