// Read native public account projections through the released authenticated web API.
// Launch credentials arrive on stdin and never leave this process.
const result = {identity: {}, windows: [], source: 'DeepSeek Harness', status: 'unavailable'};
try {
  const call = await accountClient(request.url);
  try {
    const ref = await apiKeyReference(call);
    const credential = (await call('credentials/describe', {refs: [ref]}))[ref];
    if (credential?.configured) {
      result.status = 'configured'; result.api_key_configured = true;
      result.identity = {auth_method: 'API key', ...await keyIdentity(request.home, ref, credential.source)};
    }
  } catch {}
  if (result.api_key_configured) { process.stdout.write(JSON.stringify(result)); process.exit(0); }
  const state = await call('account/getState');
  if (state.status === 'signed-out') result.status = 'signed_out';
  else {
    const client = {version: '0.2.1-rc.2', locale: 'en', timezoneOffsetSeconds: -new Date().getTimezoneOffset() * 60};
    const [profile, balance] = await Promise.allSettled([call('account/getProfile', {client}), call('account/getBalance', {client})]);
    const clean = v => typeof v === 'string' ? v.replace(/[\x00-\x1f\x7f]/g, '').slice(0,256) : '';
    if (profile.value?.status === 'ready') result.identity = {name: clean(profile.value.value.name), email: clean(profile.value.value.contact), auth_method: 'DeepSeek account'};
    if (balance.value?.status === 'ready') {
      result.balances = [...balance.value.value.map(v => ({...v,kind:'Wallet'})), ...(balance.value.bonusWallets ?? []).map(v => ({...v,kind:'Bonus'}))]
        .filter(v => ['USD','CNY'].includes(v.currency) && /^-?\d+(\.\d+)?$/.test(v.balance))
        .map(v => ({currency:v.currency,balance:v.balance,kind:v.kind}));
      result.status = 'ok';
    }
  }
} catch {}
process.stdout.write(JSON.stringify(result));
