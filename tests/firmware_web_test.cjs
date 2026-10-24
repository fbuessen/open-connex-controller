const {readFileSync} = require('node:fs');
const {Script,createContext} = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const root = path.join(__dirname,'../src/firmware/web');
const html = readFileSync(path.join(root,'index.html'),'utf8');
const script = readFileSync(path.join(root,'app.js'),'utf8');
const context = createContext({module:{exports:{}},console});
new Script(script,{filename:'firmware-app.js'}).runInContext(context);
const {ZoneWriter,NetworkFlow,validTarget,convertedTarget,MAX_ZONES,nextTabIndex} = context.module.exports;
assert.equal(MAX_ZONES,8);
assert.equal(nextTabIndex('ArrowLeft',0,4),3);
assert.equal(nextTabIndex('ArrowRight',3,4),0);
assert.equal(nextTabIndex('End',0,4),3);
assert.equal(nextTabIndex('Home',3,4),0);
assert.equal(nextTabIndex('Enter',1,4),-1);
assert(html.includes('href="/favicon.svg" type="image/svg+xml"'));
const favicon = readFileSync(path.join(root,'favicon.svg'),'utf8');
assert(favicon.includes('viewBox="0 0 64 64"') && !/<script|href=|onload=/i.test(favicon));
assert(!html.includes('zone-edit') && !html.includes('class="btn quiet logout"'));
const serverSource = readFileSync(path.join(root,'../http_server.cpp'),'utf8');
assert(serverSource.includes('connex::kSessionSeconds') && !serverSource.includes('Max-Age=28800'), 'Cookie lifetime follows server session policy');
assert(!script.includes('.innerHTML') && !script.includes('localStorage') && !script.includes('sessionStorage'));
assert(!html.includes('·') && !html.includes('connex-demo') && !html.includes('controller_demo'));
assert(!html.includes('style=') && !html.includes('<script>'), 'CSP must not need inline scripts/styles');
assert(!html.includes('<datalist'));
assert(html.includes('current settings every 5 seconds.'), 'Pairing guidance matches the radio interval');
assert(html.includes('id="wifi-networks"') && html.includes('id="setup-networks"'));
assert(script.includes("'Saved to controller.'") && script.includes('clearTimeout(timeout)'));
assert(script.includes("credentials:'same-origin'") && script.includes('X-Connex-Nonce'));
assert(!script.includes('Authorization'), 'Browser uses HttpOnly sessions, not its displayed API token');
assert(script.includes('created = true'), 'A partial zone creation must be reconciled, not blindly repeated');
assert(html.includes('id="login-description"'), 'Unconfigured normal-LAN clients get setup-button guidance');
assert(!script.includes('Create your web password') && !script.includes('Save password'), 'No password-only enrollment screen');
assert(script.includes('if (setupSubmitting || !state.portal) return;'), 'Setup submission is gated on active setup mode');
function deferred() { let resolve,reject; const promise = new Promise((yes,no) => {resolve=yes;reject=no;}); return {promise,resolve,reject}; }
async function networkTests() {
  const state = {scanning:false,networks:[],calls:[],resultGate:null,postGate:null,failResults:false,status:'setup'};
  let changes = 0;
  const flow = new NetworkFlow(async (url,method='GET',body) => {
    state.calls.push({url,method,body});
    if (url === '/ui/network' && method === 'GET') return {state:state.status,scanning:state.scanning};
    if (url === '/ui/networks') {
      if (state.failResults) { state.failResults=false; throw Error('lost result'); }
      const value = {networks:state.networks,scanning:state.scanning};
      if (state.resultGate) { const gate=state.resultGate;state.resultGate=null;await gate.promise; }
      return value;
    }
    if (url === '/ui/network/scan') { if (state.postGate) await state.postGate.promise; state.scanning=true; return null; }
    if (url === '/ui/network' && method === 'POST') { state.status='connecting'; return null; }
    throw Error('Unexpected URL');
  }, () => ++changes);
  assert(flow.busy()); await flow.poll(); assert(!flow.busy());
  const post=deferred(); state.postGate=post;
  const scan=flow.scan(); assert(flow.busy()); await flow.poll(); assert(flow.busy(),'Polling cannot unlock scan submission');
  post.resolve(); await scan; state.postGate=null;
  state.scanning=false; state.networks=[{ssid:'<unsafe & text>',rssi:-40,secured:true}];
  await flow.poll(); assert(!flow.busy()); assert.equal(flow.networks[0].ssid,'<unsafe & text>');
  await flow.scan(); state.scanning=false; state.networks=[]; await flow.poll(); assert.equal(flow.networks.length,0);
  await flow.scan(); state.scanning=false; state.failResults=true;
  await assert.rejects(flow.poll()); assert(flow.pending); await flow.poll(); assert(!flow.pending);
  await flow.scan(); state.scanning=false; const stale=deferred();state.resultGate=stale;
  const old=flow.poll(); await new Promise(setImmediate); assert(!flow.busy());
  await flow.scan(); stale.resolve(); await old; assert(flow.scanning,'Stale scan completion cannot replace the current scan');
  state.scanning=false; await flow.poll();
  await flow.connect({ssid:'Hidden',password:'12345678'}); assert(flow.busy(),'Submission stays locked until status reconciliation');
  await flow.poll(); assert(flow.busy());
  await assert.rejects(flow.connect({ssid:'Duplicate',password:''}));
  assert.equal(state.calls.filter(c=>c.url==='/ui/network' && c.method==='POST').length,1);
  state.status='setup';await flow.poll();assert(!flow.busy());assert(changes>5);
}
async function zoneTests() {
  const gate=deferred(), calls=[], errors=[];let saved=0;
  const writer=new ZoneWriter(async(id,fields)=>{calls.push({id,fields});if(calls.length===1)await gate.promise;},()=>++saved,(id,e)=>errors.push(e));
  const first=writer.queue('zone',{target_temperature:18});
  writer.queue('zone',{target_temperature:18.5});writer.queue('zone',{target_temperature:19});writer.queue('zone',{target_temperature:19.5,mode:'eco'});
  assert(writer.busy('zone') && calls.length===1);gate.resolve();await first;
  assert.equal(calls.length,2);assert.equal(calls[1].fields.target_temperature,19.5);assert.equal(calls[1].fields.mode,'eco');
  assert(!writer.busy('zone') && saved===1 && !errors.length);
  const failure=deferred();let sends=0;
  const failed=new ZoneWriter(async()=>{++sends;await failure.promise;},()=>assert.fail(),(id,e)=>errors.push(e));
  const pending=failed.queue('zone',{target_temperature:20});failed.queue('zone',{target_temperature:21});failure.reject(Error('timeout'));await pending;
  assert.equal(sends,1,'Never resend after an ambiguous mutation');assert.equal(errors.length,1);
  assert(validTarget(21.5,'celsius') && !validTarget(21.1,'celsius') && !validTarget(40,'celsius') && !validTarget(null,'celsius'));
  assert.equal(convertedTarget(20,'celsius','fahrenheit'),68);assert.equal(convertedTarget(null,'celsius','fahrenheit'),null);
}
(async()=>{await networkTests();await zoneTests();console.log('PASS: embedded script/CSP guards, VM asynchronous scan races/retry/locks, serialized latest-zone writes/no blind retries, unit conversion');})().catch(e=>{console.error(e);process.exitCode=1;});
