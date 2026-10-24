#!/usr/bin/env node
// Production assets against a loopback fixture. Never contacts the device.
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {mkdtempSync,writeFileSync} = require('node:fs');
const {tmpdir} = require('node:os');
const {join,resolve} = require('node:path');
const {server,state:fixture} = require('../tests/firmware_browser_fixture.cjs')(resolve(__dirname,'../src/firmware/web'));
const chromium = process.argv[2] || 'chromium';
const artifacts = mkdtempSync(join(tmpdir(), 'connex-firmware-browser-'));
console.log('Browser artifacts:', artifacts);
const browser = spawn(chromium, ['--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--disable-background-networking', '--disable-component-update', '--disable-sync', '--disable-dev-shm-usage',
  '--remote-debugging-port=0', '--remote-debugging-address=127.0.0.1',
  '--user-data-dir=' + join(artifacts, 'browser-profile'), 'about:blank'], {stdio:['ignore','ignore','pipe']});
let websocket, stderr = '', nextId = 1;
const pending = new Map(), exceptions = [];
const sleep = ms => new Promise(done => setTimeout(done, ms));
const boot = new Promise((done, reject) => {
  const timer = setTimeout(() => reject(new Error('Browser startup timed out: ' + stderr.slice(-3000))), 25000);
  browser.on('error', error => { clearTimeout(timer); reject(error); });
  browser.on('exit', code => { clearTimeout(timer); reject(new Error(`Browser exited ${code}: ${stderr.slice(-3000)}`)); });
  browser.stderr.on('data', chunk => {
    stderr += chunk.toString();
    const match = stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);
    if (match) { clearTimeout(timer); done(match[1]); }
  });
});
function command(method, params = {}, sessionId) {
  const id = nextId++;
  return new Promise((done, reject) => {
    const timeout = setTimeout(() => { pending.delete(id); reject(new Error(method + ' timed out')); }, 12000);
    pending.set(id, {done, reject, timeout});
    websocket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  });
}
async function main() {
  await new Promise(done => server.listen(0, '127.0.0.1', done));
  const base = 'http://127.0.0.1:' + server.address().port;
  websocket = new WebSocket(await boot);
  await new Promise((done, reject) => { websocket.addEventListener('open', done, {once:true}); websocket.addEventListener('error', reject, {once:true}); });
  websocket.addEventListener('message', event => {
    const value = JSON.parse(event.data);
    if (value.method === 'Runtime.exceptionThrown') exceptions.push(value.params.exceptionDetails);
    const callback = pending.get(value.id);
    if (!callback) return;
    pending.delete(value.id); clearTimeout(callback.timeout);
    if (value.error) callback.reject(new Error(JSON.stringify(value.error))); else callback.done(value.result);
  });
  const {targetId} = await command('Target.createTarget', {url:'about:blank'});
  const {sessionId} = await command('Target.attachToTarget', {targetId, flatten:true});
  const page = (method, params) => command(method, params, sessionId);
  await page('Runtime.enable'); await page('Page.enable');
  await page('Page.bringToFront');
  await page('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'}]});
  async function evaluate(expression) {
    const result = await page('Runtime.evaluate', {expression, awaitPromise:true, returnByValue:true});
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async function size(width, height) { await page('Emulation.setDeviceMetricsOverride', {width,height,deviceScaleFactor:1,mobile:false}); }
  async function waitUntil(expression, message) {
    for (let i = 0; i < 200; i++) {
      if (await evaluate(expression)) return;
      await sleep(100);
    }
    throw new Error(message + ': ' + await evaluate("document.getElementById('toast')?.textContent") + '; exceptions=' + JSON.stringify(exceptions));
  }
  async function screenshot(name) {
    await sleep(250);
    const {data} = await page('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
    writeFileSync(join(artifacts,name+'.png'),Buffer.from(data,'base64'));
  }
  async function waitFixture(predicate) {
    for (let i=0;i<100;i++) {if(predicate())return;await sleep(20);}
    throw new Error('Fixture request did not arrive');
  }
  async function load() {
    await page('Page.navigate',{url:'about:blank'});
    await waitUntil("location.href === 'about:blank'",'Navigation');
    await page('Page.navigate',{url:base});
    await waitUntil("document.getElementById('loading-view')?.hidden === true",'UI startup');
  }
  async function click(id) { await evaluate("document.getElementById("+JSON.stringify(id)+").click()"); }
  async function fill(form, fields) {
    await evaluate("Object.entries("+JSON.stringify(fields)+").forEach(([k,v])=>document.getElementById("+JSON.stringify(form)+").elements[k].value=v)");
  }
  async function submit(id) { await evaluate("document.getElementById("+JSON.stringify(id)+").requestSubmit()"); }
  async function settings(tab) { await evaluate("document.querySelector('[data-page=settings]').click()"); await click('tab-'+tab); }
  if (process.argv.includes('--screenshots')) {
    // Documentation uses unchanged production assets and synthetic fixture data.
    // Never use device credentials or a real controller for these screenshots.
    fixture.configured=true;fixture.password='screenshot-password';
    fixture.icons=Object.fromEntries(fixture.zones.map((zone,index)=>[zone.id,['sofa','bed','desk'][index]]));
    await size(1280,820);await load();
    await fill('login-form',{password:fixture.password});await submit('login-form');
    await waitUntil("document.querySelectorAll('.zone-card').length===3 && !document.getElementById('refresh').disabled",'Screenshot dashboard');
    await screenshot('dashboard');
    await settings('zones');await screenshot('zones');
    await settings('security');
    await waitUntil("!document.getElementById('generate-token').disabled",'Screenshot security');
    await screenshot('security');
    fixture.configured=false;fixture.authenticated=false;fixture.portal=true;fixture.status='setup';
    await size(1280,960);await load();
    await waitUntil("!document.getElementById('setup-connect').disabled",'Screenshot setup');
    await screenshot('setup');
    assert.equal(exceptions.length,0,JSON.stringify(exceptions));
    console.log('PASS: documentation screenshots of production UI with example data');
    return;
  }
  await size(1440,1000);await load();
  assert.equal(await evaluate("document.querySelector('link[rel=icon]').getAttribute('href')"),'/favicon.svg');
  assert(await evaluate("fetch('/favicon.svg').then(async r=>r.status===200 && r.headers.get('Content-Type')==='image/svg+xml' && (await r.text()).includes('<svg'))"),'Favicon is available before sign-in');
  assert(await evaluate("document.getElementById('setup-view').hidden && !document.getElementById('login-view').hidden && document.getElementById('login-form').hidden"),'No password-only enrollment on home Wi-Fi');
  assert(await evaluate("document.getElementById('login-description').textContent.includes('Setup button')"));
  assert.equal(await evaluate("fetch('/ui/session').then(r=>r.json()).then(s=>s.enrollment_allowed)"),false);
  assert.equal(await evaluate("fetch('/ui/enroll',{method:'POST',headers:{'Content-Type':'application/json','X-Connex-Nonce':'nonce'},body:JSON.stringify({password:'initial-password'})}).then(r=>r.status)"),403);
  assert(!fixture.configured);
  fixture.portal=true;fixture.status='setup';await load();
  assert(await evaluate("!document.getElementById('setup-view').hidden && !document.getElementById('setup-wifi-panel').hidden && !document.getElementById('setup-password-panel').hidden"),'Setup combines Wi-Fi and password');
  await waitUntil("!document.getElementById('setup-connect').disabled",'Setup ready');
  const enrollmentsBefore=fixture.calls.filter(c=>c.url==='/ui/enroll').length;
  await fill('setup-form',{ssid:'Home & <guest>',wifiPassword:'wifi-password',webPassword:'initial-password',confirmation:'mismatch-password'});await submit('setup-form');
  assert(await evaluate("document.getElementById('setup-error').textContent.includes('match')"));
  // Exceed the old 12-second browser deadline without triggering a blind retry.
  fixture.authDelay=13000;
  await fill('setup-form',{confirmation:'initial-password'});await submit('setup-form');
  await waitUntil("!document.getElementById('setup-success').hidden",'Enroll and save Wi-Fi');
  fixture.authDelay=0;
  assert.equal(fixture.calls.filter(c=>c.url==='/ui/enroll').length,enrollmentsBefore+1,'Slow enrollment is not resubmitted');
  assert.equal(await evaluate("document.getElementById('setup-address').textContent"),'http://open-connex.local/');
  fixture.portal=false;fixture.status='configured';await load();
  await waitUntil("document.querySelectorAll('.zone-card').length===3",'Load zones after setup');
  assert.equal(await evaluate('document.cookie'),'','Session must be HttpOnly');
  await waitUntil("!document.getElementById('refresh').disabled",'Initial refresh finishes');
  fixture.holdZonesRead=true;await click('refresh');await waitFixture(()=>fixture.releaseZonesRead);
  assert(await evaluate("[...document.querySelectorAll('.zone-card input, .zone-card button, .zone-row, #add-zone')].every(el=>el.disabled)"),'Refresh excludes edits that could be overwritten by its response');
  fixture.releaseZonesRead();await waitUntil("!document.getElementById('refresh').disabled",'Read unlocks controls');
  await screenshot('dashboard-desktop');
  assert(await evaluate("!document.querySelector('#page-zones #add-zone, #page-zones .zone-edit, .sidebar #sign-out')"),'Administration lives only in Settings');
  await evaluate("document.querySelector('.zone-card [data-mode=eco]').click()");
  await waitUntil("document.querySelector('.zone-card input').value==='18.0'",'Effective Eco temperature');
  assert.equal(await evaluate("document.querySelector('.zone-card .regular-temperature').textContent"),'21.0°C');
  await evaluate("document.querySelector('.zone-card input').value='19';document.querySelector('.zone-card input').dispatchEvent(new Event('change'))");
  await sleep(200);assert.equal(fixture.zones[0].configuration.target_temperature,22);
  await evaluate("document.querySelector('.zone-card [data-step=\"0.5\"]').click();document.querySelector('.zone-card [data-step=\"0.5\"]').click();document.querySelector('.zone-card [data-step=\"0.5\"]').click()");
  await sleep(350);assert.equal(fixture.zones[0].configuration.target_temperature,23.5);
  await settings('zones');await screenshot('settings-zones-desktop');
  assert.equal(await evaluate("document.getElementById('zone-count').textContent"),'3 of 8 zones');
  await evaluate("document.getElementById('tab-zones').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowLeft',bubbles:true}))");
  assert.equal(await evaluate('document.activeElement.id'),'tab-controller');
  await evaluate("document.getElementById('tab-controller').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight',bubbles:true}))");
  assert.equal(await evaluate('document.activeElement.id'),'tab-zones');
  await evaluate("document.querySelector('.zone-row').click()");
  await fill('zone-form',{name:'Living <room>',icon:'kitchen',unit:'fahrenheit',offset:-2});
  await evaluate("document.getElementById('zone-form').elements.unit.dispatchEvent(new Event('change'))");
  assert(await evaluate("!document.getElementById('unit-target-note').hidden"));
  await screenshot('zone-settings-desktop');await submit('zone-form');
  await waitUntil("!document.getElementById('zone-dialog').open",'Save zone');
  await sleep(250);assert.equal(fixture.zones[0].configuration.target_temperature,74.5);
  assert.equal(fixture.icons[fixture.zones[0].id],'kitchen');
  assert.equal(await evaluate("document.querySelector('.zone-title').textContent"),'Living <room>');
  assert(!await evaluate("document.querySelector('.zone-title room')"));
  await click('add-zone');await fill('zone-form',{name:'Hallway',icon:'hallway'});await submit('zone-form');
  await waitUntil("document.querySelectorAll('.zone-card').length===4",'Create silent zone');
  assert.deepEqual(fixture.zones[3].configuration,{target_temperature:20,unit:'celsius',mode:'normal',eco_offset:0});
  assert(!fixture.calls.some(c=>c.url.endsWith(fixture.zones[3].id)&&c.method==='PATCH'));
  await waitUntil("!document.getElementById('refresh').disabled",'Create refresh finishes');
  assert.equal(await evaluate("document.querySelectorAll('.zone-card input')[3].value"),'20.0');
  await evaluate("document.querySelectorAll('.zone-row')[3].click()");
  assert(await evaluate("!document.getElementById('zone-form').elements.unit.closest('label').hidden"));
  assert.equal(await evaluate("document.getElementById('zone-form').elements.offset.value"),'0');
  await click('pair-zone');await click('start-pair');
  await waitUntil("document.getElementById('start-pair').hidden",'Pair new zone without a temperature edit');
  assert.equal(fixture.calls.filter(c=>c.url===`/ui/api/v1/zones/${fixture.zones[3].id}/pairing`&&c.method==='POST').length,1);
  await click('stop-pair');await waitUntil("!document.getElementById('start-pair').hidden",'Stop new-zone pairing');
  await evaluate("document.getElementById('pair-dialog').close()");
  await evaluate("document.querySelector('.zone-row').click()");await click('pair-zone');await click('start-pair');
  await waitUntil("document.getElementById('start-pair').hidden",'Start pairing window');
  await evaluate("document.getElementById('pair-dialog').close();document.querySelectorAll('.zone-row')[1].click()");await click('pair-zone');
  assert(!await evaluate("document.getElementById('start-pair').hidden"),'Pairing countdown belongs to its zone');
  await evaluate("document.getElementById('pair-dialog').close();document.querySelector('.zone-row').click()");await click('pair-zone');await click('stop-pair');
  await waitUntil("!document.getElementById('start-pair').hidden",'Stop original pairing window');
  await evaluate("document.getElementById('pair-dialog').close()");
  fixture.holdTokenRead=true;
  await settings('security');
  await waitFixture(()=>fixture.releaseTokenRead);
  await click('generate-token');
  await waitUntil("document.getElementById('token-value').value.length===64",'Inline generated token');
  fixture.releaseTokenRead();await sleep(200);
  assert.equal(await evaluate("document.getElementById('token-value').value"),fixture.token,'Stale read cannot hide a new token');
  const token=fixture.token;await click('generate-token');await sleep(200);assert.notEqual(fixture.token,token);
  assert(!await evaluate("document.querySelector('dialog[open]')"));
  await screenshot('security-desktop');
  await click('revoke-token');await click('confirm-revoke');await sleep(200);assert.equal(fixture.token,'');
  await click('change-password');
  fixture.authDelay=13000;
  await fill('password-form',{current:'initial-password',newPassword:'replacement-password',confirmation:'replacement-password'});await submit('password-form');
  await waitUntil("!document.getElementById('login-view').hidden",'Password change signs out');
  fixture.authDelay=0;
  const passwordAt=fixture.calls.findLast(c=>c.url==='/ui/password').at;
  assert(!fixture.calls.some(c=>c.url==='/ui/network' && c.at>passwordAt+100),'Background polling pauses while hashing');
  assert.equal(await evaluate("document.getElementById('token-value').value"),'');
  await screenshot('login-desktop');
  await fill('login-form',{password:'replacement-password'});await submit('login-form');
  await waitUntil("document.querySelectorAll('.zone-card').length===4",'Sign in after password change');
  for(const width of [390,320]) {
    await size(width,844);await evaluate("document.querySelector('[data-page=zones]').click()");
    assert(await evaluate("document.documentElement.scrollWidth<=innerWidth"),'No mobile overflow');
    assert(await evaluate("document.getElementById('sign-out').getClientRects().length===0"),'No dashboard sign-out');
    if(width===390)await screenshot('dashboard-mobile');
    await settings('zones');
    assert(await evaluate("document.querySelector('.content').scrollWidth<=document.querySelector('.content').clientWidth"),'Settings zones fit mobile');
    if(width===390)await screenshot('settings-zones-mobile');
    await settings('security');await evaluate("document.getElementById('sign-out').scrollIntoView({block:'end'})");
    await screenshot('security-layout-'+width);
    const signOut = await evaluate("(()=>{const b=document.getElementById('sign-out'),r=b.getBoundingClientRect();return {top:r.top,bottom:r.bottom,height:innerHeight,inSecurity:!!b.closest('#settings-security')}})()");
    assert(signOut.top>=0 && signOut.bottom<=signOut.height+1 && signOut.inSecurity,'Sign-out reachable inside Security: '+JSON.stringify(signOut));
    if(width===390)await screenshot('security-mobile');
  }
  await click('sign-out');await waitUntil("!document.getElementById('login-view').hidden",'Security sign-out');
  assert(await evaluate("!document.querySelector('.zone-row, .zone-card') && !document.getElementById('token-value').value"),'Private zone lists and token cleared');
  await fill('login-form',{password:'replacement-password'});await submit('login-form');
  await waitUntil("document.querySelectorAll('.zone-card').length===4 && !document.getElementById('refresh').disabled",'Sign in again');
  await size(1440,1000);
  await evaluate("document.querySelector('[data-page=zones]').click()");
  fixture.dropPatch=true;
  const prior=fixture.calls.filter(c=>c.method==='PATCH'&&c.url.includes('/zones/')).length;
  await evaluate("document.querySelector('.zone-card [data-step=\"0.5\"]').click()");
  await waitUntil("document.querySelector('.zone-card input').disabled",'Ambiguous mutation locks zone');
  await sleep(300);
  assert.equal(fixture.calls.filter(c=>c.method==='PATCH'&&c.url.includes('/zones/')).length,prior+1,'No automatic resend');
  await click('refresh');await waitUntil("!document.querySelector('.zone-card input').disabled",'Read reconciles uncertain change');
  await settings('zones');
  for(let count=5;count<=8;count++) {
    await click('add-zone');await fill('zone-form',{name:'Extra zone '+count});await submit('zone-form');
    await waitUntil("document.querySelectorAll('.zone-row').length==="+count+" && !document.getElementById('refresh').disabled && !document.getElementById('zone-dialog').open",'Create zone '+count);
  }
  assert(await evaluate("document.getElementById('add-zone').disabled && document.getElementById('zone-count').textContent==='8 of 8 zones'"));
  for(let count=8;count>4;count--) {
    await evaluate("document.querySelector('.zone-row:last-child').click()");await click('delete-zone');await click('confirm-delete');
    await waitUntil("document.querySelectorAll('.zone-row').length==="+(count-1)+" && !document.getElementById('refresh').disabled && !document.getElementById('delete-dialog').open",'Delete extra zone');
  }
  assert(!await evaluate("document.getElementById('add-zone').disabled"));
  // Simulated physical reset: preserve zones; return to Wi-Fi/password setup.
  fixture.configured=false;fixture.authenticated=false;fixture.portal=true;fixture.status='setup';fixture.error='';fixture.failWifi=true;
  await load();assert(!await evaluate("document.getElementById('setup-wifi-panel').hidden"));
  await screenshot('setup-desktop');
  await waitUntil("!document.getElementById('setup-scan').disabled",'Setup network ready');await click('setup-scan');
  await waitUntil("!document.getElementById('setup-networks').disabled",'Scan results');
  await evaluate("document.getElementById('setup-networks').value='Home & <guest>';document.getElementById('setup-networks').dispatchEvent(new Event('change'))");
  assert.equal(await evaluate("document.getElementById('setup-ssid').value"),'Home & <guest>');
  await fill('setup-form',{wifiPassword:'wifi-password',webPassword:'setup-password',confirmation:'setup-password'});await submit('setup-form');
  await waitUntil("document.getElementById('setup-error').textContent.includes('Connection failed')",'Failed setup allows retry');
  assert(fixture.configured,'Password persists if Wi-Fi trial fails');
  assert(await evaluate("document.getElementById('setup-password-panel').hidden"));
  fixture.failWifi=false;
  await fill('setup-form',{wifiPassword:'wifi-password'});await submit('setup-form');
  await waitUntil("!document.getElementById('setup-success').hidden",'Setup retry succeeds');
  assert.equal(fixture.zones.length,4,'Setup preserves zones');
  await screenshot('setup-success');
  // If the portal closes between polls, reconcile the now-private status route
  // to the sign-in screen rather than leaving an unusable setup form.
  fixture.portal=false;fixture.status='configured';fixture.authenticated=false;
  await waitUntil("!document.getElementById('login-view').hidden",'Closed portal requires sign-in');
  assert.equal(exceptions.length,0,JSON.stringify(exceptions));
  console.log('PASS: real-browser production UI, setup-only enrollment, friendly hostname, HttpOnly login, password change, inline tokens, zone controls/icons/conversion, safe names, uncertain writes, mobile sign-out, setup scan/failure/retry');
  console.log('Screenshots:',artifacts);
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
  if(websocket?.readyState===WebSocket.OPEN){try{await command('Browser.close');}catch{}websocket.close();}
  if(browser.exitCode===null)browser.kill('SIGTERM');
  server.closeAllConnections();server.close();
});
