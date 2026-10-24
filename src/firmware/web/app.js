'use strict';

// Testable asynchronous state machines; mutations are never retried after an
// ambiguous response. The controller owns the ten-second RF grace period.
class ZoneWriter {
  constructor(send, changed, failed) { this.send = send; this.changed = changed; this.failed = failed; this.jobs = new Map(); }
  busy(id) { return this.jobs.has(id); }
  queue(id, fields) {
    const existing = this.jobs.get(id);
    if (existing) { Object.assign(existing.next, fields); return existing.promise; }
    const job = {next:{...fields}}; this.jobs.set(id, job);
    job.promise = this.run(id, job); return job.promise;
  }
  async run(id, job) {
    try {
      while (Object.keys(job.next).length) {
        const fields = job.next; job.next = {};
        await this.send(id, fields);
      }
      this.changed(id);
    } catch (error) { job.next = {}; this.failed(id, error); }
    finally { this.jobs.delete(id); }
  }
}
class NetworkFlow {
  constructor(request, changed) {
    this.request = request; this.changed = changed; this.generation = 0;
    this.status = null; this.networks = []; this.starting = false; this.submitting = false;
    this.scanning = false; this.pending = true; this.polling = false; this.error = '';
  }
  busy() { return !this.status || this.starting || this.submitting || this.scanning || !['setup','configured'].includes(this.status.state); }
  async poll() {
    if (this.polling) return;
    this.polling = true;
    try {
      const generation = this.generation;
      const status = await this.request('/ui/network');
      if (generation !== this.generation) return;
      this.status = status;
      if (generation === this.generation && !this.starting) this.scanning = status.scanning;
      if (this.scanning) this.pending = true;
      this.changed(this);
      if (this.pending && !this.starting) {
        const resultGeneration = this.generation;
        const result = await this.request('/ui/networks');
        if (resultGeneration !== this.generation || this.starting) return;
        this.networks = result.networks; this.scanning = result.scanning; this.pending = this.scanning;
        this.error = ''; this.changed(this);
      }
    } finally { this.polling = false; }
  }
  async scan() {
    if (this.busy()) return;
    this.starting = true; this.pending = true; ++this.generation; this.error = ''; this.changed(this);
    try { await this.request('/ui/network/scan', 'POST', {}); this.scanning = true; }
    catch (error) { this.error = error.message; throw error; }
    finally { this.starting = false; this.changed(this); }
  }
  async connect(value) {
    if (this.busy()) throw new Error('Wait for the current network operation to finish.');
    ++this.generation; this.submitting = true; this.changed(this);
    try { await this.request('/ui/network', 'POST', value); }
    finally {
      // Keep competing submissions locked until a status read reconciles them.
      this.status = null; this.submitting = false; this.changed(this);
    }
  }
}
function limits(unit) { return unit === 'celsius' ? [10,30] : [50,86]; }
function validTarget(value, unit) { const [low,high] = limits(unit); return Number.isFinite(value) && value >= low && value <= high && Number.isInteger(value * 2); }
function convertedTarget(target, from, to) { return target === null || from === to ? target : Math.round((to === 'celsius' ? (target-32)*5/9 : target*9/5+32)*2)/2; }
const MAX_ZONES = 8;
function nextTabIndex(key, at, count) { return key === 'ArrowRight' ? (at+1)%count : key === 'ArrowLeft' ? (at+count-1)%count : key === 'Home' ? 0 : key === 'End' ? count-1 : -1; }

if (typeof module !== 'undefined') module.exports = {ZoneWriter, NetworkFlow, limits, validTarget, convertedTarget, MAX_ZONES, nextTabIndex};
if (typeof document !== 'undefined') startInterface();

function startInterface() {
  const $ = id => document.getElementById(id);
  const state = {authenticated:false, configured:true, portal:false, nonce:'', page:'zones', tab:'zones', zones:[], icons:{}, token:'', editing:null, pairing:null, pairingWindows:new Map(), uncertain:new Set()};
  let noticeTimer, refreshBusy = false, stopped = false, booting = false, setupSubmitting = false, tokenBusy = false, tokenVersion = 0, setupSaved = false, passwordWork = 0;
  const flow = new NetworkFlow(request, renderNetwork);
  const writer = new ZoneWriter((id, value) => request('/ui/api/v1/zones/' + id, 'PATCH', value),
    () => notice('Saved to controller.'), (id,error) => { state.uncertain.add(id); renderZones(); notice(error.message, true); });
  function notice(message, error = false) {
    clearTimeout(noticeTimer); $('toast').textContent = message; $('toast').className = 'toast' + (error ? ' error' : '');
    if (!error) noticeTimer = setTimeout(() => { $('toast').textContent = ''; }, 5000);
  }
  function closeDialogs() { for (const dialog of document.querySelectorAll('dialog[open]')) dialog.close(); }
  function clearPrivate() {
    ++tokenVersion; state.pairingWindows.clear();
    state.authenticated = false; state.token = ''; state.zones = []; state.icons = {}; state.uncertain.clear();
    $('zone-grid').replaceChildren(); $('zone-list').replaceChildren(); $('zone-count').textContent = '';
    $('token-value').value = ''; $('token-display').hidden = true;
    for (const input of document.querySelectorAll('input[type=password]')) input.value = '';
    closeDialogs();
  }
  async function request(path, method = 'GET', body) {
    const passwordRequest = method === 'POST' && ['/ui/enroll','/ui/login','/ui/password'].includes(path);
    if (passwordRequest) ++passwordWork;
    const abort = new AbortController(), timeout = setTimeout(() => abort.abort(), passwordRequest ? 65000 : 12000);
    try {
      const response = await fetch(path, {method, credentials:'same-origin', cache:'no-store', signal:abort.signal,
        headers:{...(body === undefined ? {} : {'Content-Type':'application/json'}), ...(method === 'GET' ? {} : {'X-Connex-Nonce':state.nonce})},
        body:body === undefined ? undefined : JSON.stringify(body)});
      const value = [202,204].includes(response.status) ? null : await response.json();
      if (!response.ok) {
        if (response.status === 401 && path !== '/ui/login' && path !== '/ui/password' && path !== '/ui/session') {
          clearPrivate();
          // A physical reset or closed setup portal can change the entry screen.
          // Reconcile by reading session state; never repeat the failed mutation.
          try { Object.assign(state, await request('/ui/session')); } catch {}
          display();
        }
        const error = new Error(value?.error?.message || 'The controller rejected the request.'); error.status = response.status; throw error;
      }
      return value;
    } catch (error) {
      if (error.status) throw error;
      throw new Error(method === 'GET' ? 'Connection interrupted. Check your Wi-Fi and refresh.' : 'Change not confirmed. Refresh to check the saved state before trying again.');
    } finally { clearTimeout(timeout); if (passwordRequest) --passwordWork; }
  }
  function display() {
    const setup = state.portal, needsSetup = !state.configured && !setup;
    $('loading-view').hidden = true; $('setup-view').hidden = !setup;
    $('app-shell').hidden = setup || !state.authenticated;
    $('login-view').hidden = setup || state.authenticated;
    $('setup-password-panel').hidden = state.configured;
    $('login-form').hidden = needsSetup;
    $('login-heading').textContent = needsSetup ? 'Setup required' : 'Sign in';
    $('login-description').textContent = needsSetup ? 'Hold the Setup button for five seconds, then join the controller’s setup Wi-Fi to configure Wi-Fi and a password.' : 'Sign in to your local controller.';
    renderNetwork();
    for (const page of ['zones','settings']) {
      $('page-' + page).hidden = state.page !== page;
      const button = document.querySelector('[data-page=' + page + ']');
      if (state.page === page) button.setAttribute('aria-current','page'); else button.removeAttribute('aria-current');
    }
  }
  async function boot() {
    if (booting) return; booting = true; stopped = false;
    try {
      const session = await request('/ui/session'); Object.assign(state, session);
      display();
      if (state.authenticated && !state.portal) await refresh();
      if (state.portal || state.authenticated) await flow.poll();
    } catch (error) { notice(error.message, true); $('retry-load').hidden = false; }
    finally { booting = false; }
  }
  $('retry-load').addEventListener('click', boot);
  async function refresh() {
    if (refreshBusy || writer.jobs.size) return;
    refreshBusy = true; $('refresh').disabled = true; renderZones();
    try {
      const device = await request('/ui/api/v1/device');
      $('device-id').textContent = device.id; $('firmware-version').textContent = device.firmware_version;
      $('device-health').textContent = device.health; $('api-url').textContent = location.origin + '/api/v1';
      const zones = await request('/ui/api/v1/zones'), icons = await request('/ui/icons');
      if (!state.authenticated) return;
      state.icons = icons.icons; state.zones = zones.zones; state.uncertain.clear(); renderZones();
    } finally { refreshBusy = false; $('refresh').disabled = false; renderZones(); }
  }
  $('refresh').addEventListener('click', () => refresh().catch(e => notice(e.message, true)));
  for (const button of document.querySelectorAll('[data-close]')) button.addEventListener('click', () => button.closest('dialog').close());
  for (const button of document.querySelectorAll('[data-page]')) button.addEventListener('click', () => {
    state.page = button.dataset.page; display(); document.querySelector('.content').scrollTop = 0;
    if (state.page === 'settings') loadToken().catch(e => notice(e.message, true));
  });
  function tab(name) {
    state.tab = name;
    for (const button of document.querySelectorAll('[data-tab]')) {
      const active = button.dataset.tab === name;
      button.setAttribute('aria-selected',String(active)); button.tabIndex = active ? 0 : -1;
      $('settings-' + button.dataset.tab).hidden = !active;
    }
  }
  const tabs = [...document.querySelectorAll('[data-tab]')];
  for (const button of tabs) {
    button.addEventListener('click', () => tab(button.dataset.tab));
    button.addEventListener('keydown', event => {
      const next = nextTabIndex(event.key, tabs.indexOf(button), tabs.length);
      if (next >= 0) { event.preventDefault(); tab(tabs[next].dataset.tab); tabs[next].focus(); }
    });
  }
  function lockForm(form, locked) { for (const field of form.elements) field.disabled = locked; }
  $('login-form').addEventListener('submit', async event => {
    event.preventDefault(); const form = event.currentTarget; if (form.querySelector('button').disabled) return;
    const password = $('login-password').value; lockForm(form, true); $('login-error').textContent = '';
    try { await request('/ui/login','POST',{password}); form.reset(); await boot(); }
    catch (error) { $('login-error').textContent = error.message; }
    finally { $('login-password').value = ''; lockForm(form, false); }
  });
  $('sign-out').addEventListener('click', async () => {
    $('sign-out').disabled = true;
    try { await request('/ui/logout','POST',{}); clearPrivate(); display(); }
    catch (error) { clearPrivate(); display(); notice('Sign-out not confirmed. The server session may still be active. Reconnect and sign in, then change your password to end all sessions.',true); }
    finally { $('sign-out').disabled = false; }
  });
  function renderZones() {
    $('zone-grid').replaceChildren(); $('zone-list').replaceChildren();
    $('zone-count').textContent = state.zones.length + ' of ' + MAX_ZONES + ' zones';
    $('add-zone').disabled = refreshBusy || state.zones.length >= MAX_ZONES;
    for (const zone of state.zones) {
      const card = $('zone-template').content.firstElementChild.cloneNode(true), config = zone.configuration;
      card.dataset.id = zone.id; card.dataset.mode = config.mode;
      card.querySelector('.zone-title').textContent = zone.name;
      card.querySelector('.room-icon').setAttribute('href','#i-' + (state.icons[zone.id] || 'sofa'));
      const input = card.querySelector('input'), offset = config.mode === 'eco' ? config.eco_offset : 0, unit = config.unit === 'celsius' ? '°C' : '°F';
      input.value = config.target_temperature === null ? '' : (config.target_temperature + offset).toFixed(1);
      input.placeholder = '—'; input.min = limits(config.unit)[0] + offset; input.max = limits(config.unit)[1] + offset;
      input.setAttribute('aria-label',zone.name + (config.mode === 'eco' ? ' effective Eco temperature' : ' temperature'));
      input.disabled = refreshBusy || state.uncertain.has(zone.id);
      card.querySelector('.unit-label').textContent = unit;
      const regular = card.querySelector('.regular-temperature'); regular.hidden = config.mode !== 'eco' || config.target_temperature === null;
      regular.textContent = config.target_temperature === null ? '' : config.target_temperature.toFixed(1) + unit;
      input.addEventListener('change', () => setTarget(zone, input.value === '' ? NaN : Number(input.value) - offset));
      for (const button of card.querySelectorAll('[data-step]')) {
        const delta = Number(button.dataset.step), value = config.target_temperature === null ? (config.unit === 'celsius' ? 21 : 70) : config.target_temperature + delta;
        button.disabled = refreshBusy || state.uncertain.has(zone.id) || !validTarget(value,config.unit);
        button.setAttribute('aria-label',(delta > 0 ? 'Increase ' : 'Decrease ') + zone.name + ' temperature');
        button.addEventListener('click', () => setTarget(zone,value));
      }
      for (const button of card.querySelectorAll('[data-mode]')) {
        button.setAttribute('aria-pressed',String(button.dataset.mode === config.mode));
        button.disabled = refreshBusy || state.uncertain.has(zone.id) || config.target_temperature === null;
        button.addEventListener('click', () => { if (refreshBusy) return; config.mode = button.dataset.mode; renderZones(); writer.queue(zone.id,{mode:config.mode}); });
      }
      $('zone-grid').append(card);
      const row = $('zone-row-template').content.firstElementChild.cloneNode(true);
      row.dataset.id = zone.id; row.disabled = refreshBusy;
      row.setAttribute('aria-label','Edit ' + zone.name);
      row.querySelector('.room-icon').setAttribute('href','#i-' + (state.icons[zone.id] || 'sofa'));
      row.querySelector('.zone-row-name').textContent = zone.name;
      row.querySelector('.zone-unit').textContent = config.unit === 'celsius' ? 'Celsius' : 'Fahrenheit';
      row.querySelector('.zone-offset').textContent = 'Eco offset ' + (config.eco_offset > 0 ? '+' : '') + config.eco_offset + unit;
      row.addEventListener('click', () => editZone(zone.id)); $('zone-list').append(row);
    }
    if (!state.zones.length) {
      for (const id of ['zone-grid','zone-list']) {
        const empty = document.createElement('p'); empty.className = 'empty';
        empty.textContent = id === 'zone-grid' ? 'No zones yet. Add one in Settings.' : 'No zones yet.';
        $(id).append(empty);
      }
    }
  }
  function setTarget(zone, value) {
    if (refreshBusy) return;
    if (!validTarget(value,zone.configuration.unit)) { renderZones(); notice('Use a supported temperature in half-degree steps.',true); return; }
    zone.configuration.target_temperature = value; renderZones(); writer.queue(zone.id,{target_temperature:value});
  }
  function editZone(id) {
    if (refreshBusy || (!id && state.zones.length >= MAX_ZONES)) return;
    if (id && (writer.busy(id) || state.uncertain.has(id))) { notice('Refresh after pending changes finish before editing this zone.',true); return; }
    state.editing = id; const zone = state.zones.find(z => z.id === id), form = $('zone-form');
    form.reset(); form.elements.name.value = zone?.name || ''; form.elements.icon.value = state.icons[id] || 'sofa';
    form.elements.unit.value = zone?.configuration.unit || 'celsius'; form.elements.offset.value = zone?.configuration.eco_offset || 0;
    for (const name of ['unit','offset']) form.elements[name].closest('label').hidden = !zone || zone.configuration.target_temperature === null;
    $('zone-dialog-title').textContent = zone ? 'Zone settings' : 'Add zone'; $('zone-extra').hidden = !zone;
    $('zone-error').textContent = ''; unitNote(); $('zone-dialog').showModal();
  }
  function unitNote() {
    const zone = state.zones.find(z => z.id === state.editing), selected = $('zone-form').elements.unit.value;
    const target = zone ? convertedTarget(zone.configuration.target_temperature,zone.configuration.unit,selected) : null;
    $('unit-target-note').hidden = !zone || selected === zone.configuration.unit || target === null;
    $('unit-target-note').textContent = 'Converted target: ' + target + (selected === 'celsius' ? '°C' : '°F') + '. Review the Eco offset in the new unit.';
  }
  $('add-zone').addEventListener('click', () => editZone(null)); $('zone-form').elements.unit.addEventListener('change',unitNote);
  $('zone-form').addEventListener('submit', async event => {
    event.preventDefault(); const form = event.currentTarget; if (form.querySelector('[type=submit]').disabled) return;
    if (refreshBusy) { $('zone-error').textContent = 'Wait for the current refresh to finish.'; return; }
    const value = {name:form.elements.name.value.trim(),unit:form.elements.unit.value,offset:Number(form.elements.offset.value),icon:form.elements.icon.value};
    const old = state.zones.find(z => z.id === state.editing);
    let created = false, saved = false; lockForm(form,true); $('zone-error').textContent = '';
    try {
      if (!value.name || !Number.isInteger(value.offset) || Math.abs(value.offset) > 9) throw new Error('Enter a name and an integer Eco offset from −9 to +9.');
      if (!old) {
        const result = await request('/ui/api/v1/zones','POST',{name:value.name}); state.editing = result.id; created = true;
        // Creation saves defaults without RF; pairing or control updates dispatch them.
      } else {
        const patch = {}; if (value.name !== old.name) patch.name = value.name;
        if (value.unit !== old.configuration.unit) {
          const target = convertedTarget(old.configuration.target_temperature,old.configuration.unit,value.unit);
          if (!validTarget(target,value.unit)) throw new Error('Set a target on the Zones screen before changing the unit.');
          patch.unit = value.unit; patch.target_temperature = target;
        }
        if (value.offset !== old.configuration.eco_offset) patch.eco_offset = value.offset;
        if (Object.keys(patch).length) await request('/ui/api/v1/zones/' + old.id,'PATCH',patch);
      }
      saved = true; await request('/ui/icons','PATCH',{id:state.editing,icon:value.icon});
      $('zone-dialog').close(); await refresh(); notice('Saved to controller.');
    } catch (error) {
      $('zone-error').textContent = (saved ? 'Zone saved; icon update needs attention. ' : created ? 'Zone was created. ' : '') + error.message;
      if (created) { $('zone-dialog').close(); notice($('zone-error').textContent,true); }
      await refresh().catch(() => {});
    } finally { lockForm(form,false); }
  });
  $('delete-zone').addEventListener('click', () => { $('zone-dialog').close(); $('delete-dialog').showModal(); });
  $('confirm-delete').addEventListener('click', async () => {
    $('confirm-delete').disabled = true;
    try { await request('/ui/api/v1/zones/' + state.editing,'DELETE'); $('delete-dialog').close(); await refresh(); notice('Zone deleted.'); }
    catch (error) { notice(error.message,true); } finally { $('confirm-delete').disabled = false; }
  });
  $('pair-zone').addEventListener('click', () => {
    state.pairing = state.editing; $('zone-dialog').close(); $('pair-dialog').showModal(); pairingStatus();
  });
  function pairingStatus() {
    const seconds = Math.max(0,Math.ceil(((state.pairingWindows.get(state.pairing) || 0)-Date.now())/1000));
    $('pair-progress').textContent = seconds ? 'Pairing window: ' + seconds + 's remaining. Verify success on the heater.' : 'Put the heater in learning mode, then start pairing.';
    $('start-pair').hidden = seconds > 0; $('stop-pair').hidden = false;
  }
  for (const [id,method,body] of [['start-pair','POST',{duration_seconds:30}],['stop-pair','DELETE',undefined]]) {
    $(id).addEventListener('click', async () => {
      const zone = state.pairing;
      $('start-pair').disabled = $('stop-pair').disabled = true;
      try { await request('/ui/api/v1/zones/' + zone + '/pairing',method,body); state.pairingWindows.set(zone,method === 'POST' ? Date.now()+30000 : 0); pairingStatus(); }
      catch (error) { notice(error.message,true); }
      finally { $('start-pair').disabled = $('stop-pair').disabled = false; }
    });
  }
  $('change-password').addEventListener('click', () => { $('password-form').reset(); $('password-error').textContent = ''; $('password-dialog').showModal(); });
  $('password-form').addEventListener('submit', async event => {
    event.preventDefault(); const form = event.currentTarget; if (form.querySelector('[type=submit]').disabled) return;
    if (form.elements.newPassword.value !== form.elements.confirmation.value) { $('password-error').textContent = 'The new passwords must match.'; return; }
    const value = {current:form.elements.current.value,password:form.elements.newPassword.value}; lockForm(form,true);
    try { await request('/ui/password','POST',value); clearPrivate(); display(); notice('Password changed. Sign in again.'); }
    catch (error) { $('password-error').textContent = error.message; }
    finally { form.reset(); lockForm(form,false); }
  });
  function renderToken() {
    const exists = !!state.token; $('token-display').hidden = !exists; $('token-value').value = state.token;
    $('token-title').textContent = exists ? 'API token active' : 'No API token yet';
    $('token-description').textContent = exists ? 'Use this token in Home Assistant or your scripts.' : 'Generate one when you’re ready to connect.';
    $('token-status').textContent = exists ? 'Active' : 'Not generated'; $('token-status').className = 'badge ' + (exists ? 'green' : 'neutral');
    $('token-action-label').textContent = exists ? 'Rotate token' : 'Generate token'; $('revoke-token').hidden = !exists;
    $('copy-status').textContent = '';
  }
  async function loadToken(reconcile = false) {
    if (tokenBusy && !reconcile) return;
    const version = tokenVersion, value = await request('/ui/token');
    if (state.authenticated && version === tokenVersion) { state.token = value.token; renderToken(); }
  }
  async function changeToken(method) {
    if (tokenBusy) return; tokenBusy = true; ++tokenVersion; $('generate-token').disabled = $('confirm-revoke').disabled = true;
    try { const value = await request('/ui/token',method,method === 'POST' ? {} : undefined); if (state.authenticated) { state.token = value.token; renderToken(); } $('revoke-dialog').close(); }
    catch (error) { notice(error.message,true); await loadToken(true).catch(() => {}); }
    finally { tokenBusy = false; $('generate-token').disabled = $('confirm-revoke').disabled = false; }
  }
  $('generate-token').addEventListener('click', () => changeToken('POST'));
  $('revoke-token').addEventListener('click', () => $('revoke-dialog').showModal());
  $('confirm-revoke').addEventListener('click', () => changeToken('DELETE'));
  $('copy-token').addEventListener('click', async () => {
    const token = state.token;
    try { await navigator.clipboard.writeText(token); if (state.authenticated && state.token === token) $('copy-status').textContent = 'Token copied.'; }
    catch { if (state.authenticated && state.token === token) { $('token-value').focus(); $('token-value').select(); $('copy-status').textContent = 'Copy the selected token manually.'; } }
  });
  function renderNetwork() {
    const status = flow.status;
    const busy = flow.busy() || !state.nonce;
    for (const [formId,selectId,scanId,statusId] of [['wifi-form','wifi-networks','scan-wifi','scan-status'],['setup-form','setup-networks','setup-scan','setup-scan-status']]) {
      const form = $(formId), setup = formId === 'setup-form';
      for (const field of form.elements) {
        const passwordField = ['webPassword','confirmation'].includes(field.name);
        field.disabled = setup ? setupSubmitting || (passwordField ? state.configured : (!state.portal || busy)) : busy;
      }
      if (setup) $('setup-connect').disabled = setupSubmitting || !state.portal || !state.nonce || busy;
      $(scanId).disabled = busy || setupSubmitting;
      const selector = $(selectId), selected = form.elements.ssid.value;
      selector.replaceChildren(new Option(flow.networks.length ? 'Choose a network' : 'Scan to find networks',''), ...flow.networks.map(n => new Option(n.ssid + ' (' + n.rssi + ' dBm' + (n.secured ? '' : ', open') + ')',n.ssid)));
      selector.value = flow.networks.some(n => n.ssid === selected) ? selected : '';
      selector.disabled = busy || setupSubmitting || !flow.networks.length;
      $(statusId).textContent = flow.error || (flow.starting || flow.scanning ? 'Scanning for nearby networks…' : flow.pending ? 'Find a network, or enter its name.' : flow.networks.length ? 'Choose a network, or enter its name.' : 'No networks found. Enter a name manually or scan again.');
    }
    if (!status) return;
    $('current-wifi').textContent = status.ssid || 'Not connected'; $('wifi-connection').textContent = status.connected ? 'Connected' : 'Disconnected';
    if (state.portal) {
      $('setup-progress').hidden = !['connecting','resetting'].includes(status.state);
      if (status.error) $('setup-error').textContent = status.error;
      if (status.state === 'saved' || status.state === 'configured') {
        setupSaved = true; $('setup-form').hidden = true; $('setup-success').hidden = false; $('setup-progress').hidden = true;
        $('setup-connected-network').textContent = status.ssid; $('setup-address').textContent = 'http://' + status.hostname + '.local/';
      }
    } else if (status.error) notice(status.error,true);
  }
  for (const [formId,selectId,scanId] of [['wifi-form','wifi-networks','scan-wifi'],['setup-form','setup-networks','setup-scan']]) {
    const form = $(formId), select = $(selectId);
    select.addEventListener('change', () => { if (select.value) form.elements.ssid.value = select.value; });
    form.elements.ssid.addEventListener('input', () => { select.value = flow.networks.some(n => n.ssid === form.elements.ssid.value) ? form.elements.ssid.value : ''; });
    $(scanId).addEventListener('click', () => flow.scan().catch(e => notice(e.message,true)));
  }
  $('wifi-form').addEventListener('submit', async event => {
    event.preventDefault(); const form = event.currentTarget;
    const value = {ssid:form.elements.ssid.value,password:form.elements.password.value};
    try { await flow.connect(value); notice('Connection attempt accepted. Keep this page open.'); }
    catch (error) { notice(error.message,true); }
    finally { form.elements.password.value = ''; await flow.poll().catch(() => {}); }
  });
  $('setup-form').addEventListener('submit', async event => {
    event.preventDefault(); if (setupSubmitting || !state.portal) return;
    const form = event.currentTarget, fields = form.elements;
    if (!state.configured && fields.webPassword.value !== fields.confirmation.value) { $('setup-error').textContent = 'The web passwords must match.'; return; }
    const password = fields.webPassword.value, wifi = {ssid:fields.ssid.value,password:fields.wifiPassword.value};
    setupSubmitting = true; renderNetwork(); $('setup-error').textContent = '';
    try {
      if (!state.configured) {
        await request('/ui/enroll','POST',{password}); state.configured = state.authenticated = true;
        $('setup-password-panel').hidden = true;
      }
      await flow.connect(wifi);
    } catch (error) { $('setup-error').textContent = error.message; }
    finally {
      fields.webPassword.value = fields.confirmation.value = fields.wifiPassword.value = '';
      setupSubmitting = false; renderNetwork(); if (state.portal) await flow.poll().catch(() => {});
    }
  });
  $('setup-finish').addEventListener('click', () => location.reload());
  setInterval(async () => {
    pairingStatus();
    if (stopped || booting || passwordWork || (!state.portal && !state.authenticated)) return;
    try { await flow.poll(); }
    catch (error) { if (!setupSaved) notice(error.message,true); }
  },2000);
  window.addEventListener('pagehide', () => { stopped = true; state.token = ''; $('token-value').value = ''; });
  window.addEventListener('pageshow', event => { if (event.persisted) { clearPrivate(); boot(); } });
  boot();
}
