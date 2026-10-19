// Front end for button-dispatcher. Plain DOM code: all dynamic text goes through textContent
// because names, URLs and log lines can come from users or Bluetooth advertisements.
'use strict';

const $ = (id) => document.getElementById(id);

// ---- small DOM helpers --------------------------------------------------------------------------

// Builds an element. opts: cls, text, attrs (object), on (object of event listeners).
function el(tag, opts = {}, ...kids) {
  const n = document.createElement(tag);
  if (opts.cls) n.className = opts.cls;
  if (opts.text !== undefined) n.textContent = opts.text;
  for (const [k, v] of Object.entries(opts.attrs || {})) n.setAttribute(k, v);
  for (const [k, v] of Object.entries(opts.on || {})) n.addEventListener(k, v);
  for (const k of kids) if (k) n.append(k);
  return n;
}

function option(value, label) {
  const o = document.createElement('option');
  o.value = value;
  o.textContent = label;
  return o;
}

function setError(node, message) {
  node.textContent = message || '';
  node.hidden = !message;
}

let toastTimer = null;
function toast(message) {
  const t = $('toast');
  t.textContent = message;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

// ---- API ----------------------------------------------------------------------------------------

class SessionEnded extends Error {
  constructor() {
    super('Your session ended. Please sign in again.');
    this.sessionEnded = true;
  }
}

// One entry point for all requests: adds the required headers, parses JSON, turns server errors
// into Error(message) with .status, and sends the user back to sign-in when the session is gone.
async function api(method, path, body) {
  const init = { method, credentials: 'same-origin', headers: {} };
  if (method !== 'GET') {
    init.headers['Content-Type'] = 'application/json';
    init.headers['X-Button-Dispatcher'] = '1';
    init.body = JSON.stringify(body === undefined ? {} : body);
  }
  let res;
  try {
    res = await fetch(path, init);
  } catch (e) {
    throw new Error('Cannot reach the server.');
  }
  let data = null;
  try { data = await res.json(); } catch (e) { /* non-JSON body */ }
  if (res.status === 401 && !path.startsWith('/api/auth/')) {
    showSignedOut('Your session ended. Please sign in again.');
    throw new SessionEnded();
  }
  if (!res.ok) {
    const err = new Error((data && data.error) || `Request failed (${res.status}).`);
    err.status = res.status;
    throw err;
  }
  return data;
}

// Disables `btn` while `fn` runs so a request cannot be started twice. With `cooldownMs`, a button whose
// action succeeded stays disabled that much longer: the device can be slow, and a repeat click right after
// the result is almost always impatience rather than intent. A failure re-enables it at once so the user
// can fix the input and retry.
async function withBusy(btn, fn, cooldownMs = 0) {
  if (btn.disabled) return undefined;
  btn.disabled = true;
  let cooling = false;
  try {
    const result = await fn();
    cooling = cooldownMs > 0;
    return result;
  } finally {
    if (cooling) setTimeout(() => { btn.disabled = false; }, cooldownMs);
    else btn.disabled = false;
  }
}

// Runs fn and shows a failure in `errNode` (or the page-level alert); session-ended is silent.
async function guarded(btn, errNode, fn, cooldownMs = 0) {
  const target = errNode || $('page-error');
  setError(target, '');
  try {
    return await withBusy(btn, fn, cooldownMs);
  } catch (e) {
    if (!e.sessionEnded) setError(target, e.message);
    return undefined;
  }
}

// ---- app state ----------------------------------------------------------------------------------

let state = null;   // last GET /api/state
const TABS = ['buttons', 'bindings', 'log', 'settings'];
let currentTab = 'buttons';

function buttonById(id) {
  return state && state.buttons.find((b) => b.id === id);
}

async function refresh() {
  state = await api('GET', '/api/state');
  renderButtons();
  renderBindings();
  renderSettings();
}

// ---- auth screens -------------------------------------------------------------------------------

function showView(name) {
  $('auth-view').hidden = name === 'app';
  $('app-view').hidden = name !== 'app';
}

function stopAllPolling() {
  stopLogPolling();
  stopLearnPolling();
}

function closeDialogs() {
  for (const d of document.querySelectorAll('dialog[open]')) d.close();
}

function showSignedOut(notice) {
  stopAllPolling();
  closeDialogs();
  state = null;
  showView('auth');
  $('setup-form').hidden = true;
  $('login-form').hidden = false;
  $('login-password').value = '';
  const n = $('auth-notice');
  n.textContent = notice || '';
  n.hidden = !notice;
  setError($('login-error'), '');
  $('login-password').focus();
}

async function init() {
  let status;
  try {
    status = await api('GET', '/api/auth/status');
  } catch (e) {
    showView('auth');
    const n = $('auth-notice');
    n.textContent = e.message;
    n.hidden = false;
    return;
  }
  if (!status.configured) {
    showView('auth');
    $('setup-form').hidden = false;
    $('login-form').hidden = true;
    $('setup-password').focus();
  } else if (!status.authenticated) {
    showSignedOut('');
  } else {
    await startApp();
  }
}

async function startApp() {
  showView('app');
  $('auth-notice').hidden = true;
  try {
    await refresh();
  } catch (e) {
    if (!e.sessionEnded) setError($('page-error'), e.message);
  }
  showTab(tabFromHash(), false);
}

$('setup-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const err = $('setup-error');
  const pw = $('setup-password').value;
  if (pw.length < 8) return setError(err, 'The password must be at least 8 characters.');
  if (pw !== $('setup-repeat').value) return setError(err, 'The two passwords do not match.');
  guarded(ev.submitter || ev.target.querySelector('button[type=submit]'), err, async () => {
    await api('POST', '/api/auth/setup', { password: pw });
    $('setup-password').value = '';
    $('setup-repeat').value = '';
    await startApp();
  });
});

$('login-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const pw = $('login-password').value;
  if (!pw) return setError($('login-error'), 'Enter the password.');
  guarded(ev.target.querySelector('button[type=submit]'), $('login-error'), async () => {
    await api('POST', '/api/auth/login', { password: pw });
    $('login-password').value = '';
    await startApp();
  });
});

$('signout').addEventListener('click', (ev) => {
  guarded(ev.currentTarget, null, async () => {
    await api('POST', '/api/auth/logout');
    showSignedOut('You are signed out.');
  });
});

// ---- tabs ---------------------------------------------------------------------------------------

function tabFromHash() {
  const h = location.hash.replace('#', '');
  return TABS.includes(h) ? h : 'buttons';
}

function showTab(name, updateHash = true) {
  currentTab = name;
  for (const t of TABS) {
    const selected = t === name;
    const tab = $('tab-' + t);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    $('panel-' + t).hidden = !selected;
  }
  if (updateHash && location.hash !== '#' + name) history.replaceState(null, '', '#' + name);
  setError($('page-error'), '');
  syncLogPolling();
}

for (const tab of document.querySelectorAll('.tabs [data-tab]')) {
  tab.addEventListener('click', () => showTab(tab.dataset.tab));
  // Arrow keys move between tabs, as expected for a tablist.
  tab.addEventListener('keydown', (ev) => {
    const i = TABS.indexOf(tab.dataset.tab);
    let next = null;
    if (ev.key === 'ArrowRight') next = TABS[(i + 1) % TABS.length];
    else if (ev.key === 'ArrowLeft') next = TABS[(i + TABS.length - 1) % TABS.length];
    if (next) {
      ev.preventDefault();
      showTab(next);
      $('tab-' + next).focus();
    }
  });
}
window.addEventListener('hashchange', () => {
  if (!$('app-view').hidden) showTab(tabFromHash(), false);
});

// ---- Buttons tab --------------------------------------------------------------------------------

function renderButtons() {
  const list = $('buttons-list');
  list.replaceChildren();
  if (!state.buttons.length) {
    list.append(el('p', { cls: 'empty', text: 'No devices yet. Click "Find device" and press your BLE button, or use "Add by pressing a button" on the Bindings tab.' }));
    return;
  }
  for (const b of state.buttons) {
    const card = el('div', { cls: 'card' },
      el('h3', { text: b.name }),
      el('p', { cls: 'meta', text: b.mac + (b.model ? ' - ' + b.model : '') }));
    const actions = el('div', { cls: 'actions' },
      el('button', { text: 'Rename', attrs: { type: 'button' }, on: { click: () => openRename(b) } }),
      el('button', { cls: 'danger', text: 'Delete', attrs: { type: 'button' },
        on: { click: (ev) => deleteButton(b, ev.currentTarget) } }));
    card.append(actions);
    list.append(card);
  }
}

let renaming = null;
function openRename(b) {
  renaming = b;
  $('rename-name').value = b.name;
  setError($('rename-error'), '');
  $('rename-dialog').showModal();
  $('rename-name').select();
}
$('rename-cancel').addEventListener('click', () => $('rename-dialog').close());
$('rename-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const name = $('rename-name').value.trim();
  if (!name) return setError($('rename-error'), 'Enter a name.');
  guarded(ev.target.querySelector('button[type=submit]'), $('rename-error'), async () => {
    await api('PUT', '/api/buttons/' + encodeURIComponent(renaming.id), { name });
    $('rename-dialog').close();
    await refresh();
    toast('Renamed.');
  });
});

async function deleteButton(b, btn) {
  const users = state.bindings.filter((x) => x.trigger.button === b.id).length;
  const text = users
    ? `Delete "${b.name}" and its ${users} binding(s)?`
    : `Delete "${b.name}"?`;
  if (!confirm(text)) return;
  guarded(btn, null, async () => {
    const base = '/api/buttons/' + encodeURIComponent(b.id);
    try {
      await api('DELETE', users ? base + '?cascade=1' : base);
    } catch (e) {
      // The binding list may have changed since the page loaded; ask again with the real situation.
      if (e.status !== 409 || !confirm(`${e.message}\n\nDelete the button and those bindings?`)) throw e;
      await api('DELETE', base + '?cascade=1');
    }
    await refresh();
    toast('Button deleted.');
  });
}

// ---- Find button dialog -------------------------------------------------------------------------

// `intent` is where the dialog was opened from: 'device' (Devices tab) or 'binding' (Bindings tab).
const learn = { gen: 0, timer: null, phase: '', saved: false, trigger: null, savedName: '', intent: 'device' };

function stopLearnPolling() {
  learn.gen++;
  clearTimeout(learn.timer);
}

function resetLearnUi() {
  learn.phase = '';
  learn.saved = false;
  learn.trigger = null;
  setError($('learn-error'), '');
  $('learn-message').textContent = 'Starting...';
  for (const id of ['learn-seconds', 'learn-candidate', 'learn-known', 'learn-name-field',
    'learn-saved', 'learn-save', 'learn-save-binding', 'learn-again', 'learn-binding']) $(id).hidden = true;
}

async function startLearning() {
  stopLearnPolling();
  resetLearnUi();
  try {
    await api('POST', '/api/learn/start');
  } catch (e) {
    if (!e.sessionEnded) {
      $('learn-message').textContent = 'Could not start.';
      setError($('learn-error'), e.message);
    }
    return;
  }
  pollLearning();
}

function pollLearning() {
  const gen = ++learn.gen;
  const tick = async () => {
    if (gen !== learn.gen) return;
    try {
      const s = await api('GET', '/api/learn');
      if (gen !== learn.gen) return;
      setError($('learn-error'), '');
      renderLearn(s);
    } catch (e) {
      if (gen !== learn.gen || e.sessionEnded) return;
      setError($('learn-error'), e.message);
    }
    if (gen === learn.gen) learn.timer = setTimeout(tick, 500);
  };
  tick();
}

function addDetail(dl, label, value) {
  dl.append(el('dt', { text: label }), el('dd', { text: String(value) }));
}

function renderLearn(s) {
  const c = s.candidate;
  $('learn-message').textContent = s.message || '';

  const secs = $('learn-seconds');
  const counting = (s.phase === 'waiting' || s.phase === 'confirm') && typeof s.seconds_left === 'number';
  secs.hidden = !counting;
  if (counting) secs.textContent = `${Math.ceil(s.seconds_left)} seconds left`;

  const dl = $('learn-candidate');
  dl.replaceChildren();
  dl.hidden = !c;
  if (c) {
    if (c.description) addDetail(dl, 'Press', c.description);
    if (c.name) addDetail(dl, 'Device', c.name);
    if (c.mac) addDetail(dl, 'Address', c.mac);
    if (typeof c.rssi === 'number') addDetail(dl, 'Signal', c.rssi + ' dBm');
    if (typeof c.battery === 'number') addDetail(dl, 'Battery', c.battery + '%');
  }

  const confirmed = s.phase === 'confirmed';
  const knownId = confirmed && c ? c.known_button : null;
  const known = knownId ? buttonById(knownId) : null;
  const knownNote = $('learn-known');
  knownNote.hidden = !knownId;
  if (knownId) {
    // A remote has several physical buttons that share one device entry, so a known device is
    // normal: say so, and go straight to a binding for the button that was pressed.
    knownNote.textContent = `This is a button on "${known ? known.name : 'a device you already added'}". `
      + 'The device stays as it is; this press gets its own binding.';
  }
  // The name field is only filled when entering the confirmed phase so polling never overwrites typing.
  $('learn-name-field').hidden = !confirmed || !!knownId;
  if (confirmed && learn.phase !== 'confirmed') {
    if (knownId) {
      $('learn-save-binding').focus();
    } else {
      $('learn-name').value = (c && c.name) || 'My remote';
      $('learn-name').focus();
    }
  }
  // A known device needs only a binding. A new device is saved together with a binding (the usual
  // reason to be here), or on its own when the dialog was opened from the Devices tab.
  $('learn-save-binding').textContent = knownId ? 'Add a binding for this press' : 'Save device and add binding';
  $('learn-save-binding').hidden = !confirmed;
  $('learn-save').hidden = !confirmed || !!knownId || learn.intent === 'binding';
  $('learn-again').hidden = s.phase !== 'timeout';
  learn.phase = s.phase;
}

function closeLearnDialog() {
  $('learn-dialog').close();
}

function openLearn(intent) {
  learn.intent = intent;
  $('learn-dialog').showModal();
  startLearning();
}
$('find-button').addEventListener('click', () => openLearn('device'));
$('add-binding-press').addEventListener('click', () => openLearn('binding'));
$('learn-close').addEventListener('click', closeLearnDialog);
$('learn-again').addEventListener('click', startLearning);
$('learn-dialog').addEventListener('close', () => {
  stopLearnPolling();
  // Leaving without saving must stop the scan on the server.
  if (!learn.saved) api('POST', '/api/learn/cancel').catch(() => {});
  refresh().catch(() => {});
});

$('learn-save').addEventListener('click', (ev) => {
  const nameInput = $('learn-name');
  const needsName = !$('learn-name-field').hidden;
  const name = nameInput.value.trim();
  if (needsName && !name) return setError($('learn-error'), 'Enter a name for the device.');
  guarded(ev.currentTarget, $('learn-error'), async () => {
    const res = await api('POST', '/api/learn/save', needsName ? { name } : {});
    stopLearnPolling();
    learn.saved = true;
    learn.trigger = res.trigger;
    await refresh();
    $('learn-message').textContent = 'Done.';
    for (const id of ['learn-seconds', 'learn-candidate', 'learn-known', 'learn-name-field', 'learn-save']) {
      $(id).hidden = true;
    }
    $('learn-saved-text').textContent = res.created
      ? `Saved "${res.button.name}".`
      : `"${res.button.name}" was already added.`;
    $('learn-saved').hidden = false;
    $('learn-binding').hidden = false;
    $('learn-binding').focus();
  });
});

// Saves the device if it is new, then opens the binding editor for the press that was confirmed.
$('learn-save-binding').addEventListener('click', (ev) => {
  const needsName = !$('learn-name-field').hidden;
  const name = $('learn-name').value.trim();
  if (needsName && !name) return setError($('learn-error'), 'Enter a name for the device.');
  guarded(ev.currentTarget, $('learn-error'), async () => {
    const res = await api('POST', '/api/learn/save', needsName ? { name } : {});
    stopLearnPolling();
    learn.saved = true;
    await refresh();
    closeLearnDialog();
    showTab('bindings');
    openBindingEditor(null, res.trigger);
  });
});

$('learn-binding').addEventListener('click', () => {
  const trigger = learn.trigger;
  closeLearnDialog();
  showTab('bindings');
  openBindingEditor(null, trigger);
});

// ---- Bindings tab -------------------------------------------------------------------------------

function inputLabel(input) {
  if (input === 'wheel:left') return 'wheel turned left';
  if (input === 'wheel:right') return 'wheel turned right';
  const m = /^button:(\d+)$/.exec(input);
  return m ? 'button ' + m[1] : input;
}

function eventLabel(value) {
  const found = state.choices.button_events.find((e) => e.value === value);
  return found ? found.label : value;
}

function triggerSummary(t) {
  const b = buttonById(t.button);
  const parts = [inputLabel(t.input)];
  if (!t.input.startsWith('wheel:')) parts.push(eventLabel(t.event));
  parts.push(t.channel === null || t.channel === undefined ? 'any channel' : 'channel ' + t.channel);
  return `${b ? b.name : 'Unknown button'} - ${parts.join(', ')}`;
}

// Webhook URLs often contain secret keys in the path or query, so only scheme and host are shown.
function safeOrigin(url) {
  try {
    const u = new URL(url);
    return `${u.protocol}//${u.host}`;
  } catch (e) {
    return '(invalid URL)';
  }
}

function actionSummary(a) {
  if (a.type === 'http') return `${a.method} ${safeOrigin(a.url)}`;
  if (a.type === 'switchbot_command') {
    return `SwitchBot ${a.command} -> ${a.device_name || a.device_id}`;
  }
  if (a.type === 'switchbot_scene') return `SwitchBot scene: ${a.scene_name || a.scene_id}`;
  return a.type;
}

function bindingPayload(b, overrides = {}) {
  return {
    name: b.name, enabled: b.enabled, cooldown_ms: b.cooldown_ms,
    trigger: b.trigger, action: b.action, ...overrides,
  };
}

function renderBindings() {
  const list = $('bindings-list');
  list.replaceChildren();
  if (!state.bindings.length) {
    list.append(el('p', { cls: 'empty', text: 'No bindings yet. A binding connects a button press to an action.' }));
    return;
  }
  for (const b of state.bindings) list.append(bindingCard(b));
}

function bindingCard(b) {
  const toggle = el('input', { attrs: { type: 'checkbox', id: 'en-' + b.id } });
  toggle.checked = b.enabled;
  toggle.addEventListener('change', () => {
    const wanted = toggle.checked;
    guarded(toggle, null, async () => {
      try {
        await api('PUT', '/api/bindings/' + encodeURIComponent(b.id), bindingPayload(b, { enabled: wanted }));
      } catch (e) {
        toggle.checked = !wanted;
        throw e;
      }
      b.enabled = wanted;
      card.classList.toggle('disabled', !wanted);
    });
  });
  const result = el('p', { cls: 'run-result', attrs: { role: 'status' } });
  result.hidden = true;

  const runBtn = el('button', { text: 'Run now', attrs: { type: 'button' } });
  runBtn.addEventListener('click', () => {
    result.hidden = true;
    const label = runBtn.textContent;
    runBtn.textContent = 'Running...';
    guarded(runBtn, null, async () => {
      try {
        const r = await api('POST', `/api/bindings/${encodeURIComponent(b.id)}/run`);
        result.textContent = r.summary;
        result.className = 'run-result ' + (r.ok ? 'ok' : 'fail');
        result.hidden = false;
      } finally {
        runBtn.textContent = label;
      }
    });
  });
  const simBtn = el('button', { text: 'Simulate press', attrs: { type: 'button' } });
  simBtn.addEventListener('click', () => {
    result.hidden = true;
    guarded(simBtn, null, async () => {
      const r = await api('POST', '/api/simulate', { ...b.trigger });
      result.textContent = r.matched.length
        ? 'Simulated press; fired: ' + r.matched.join(', ')
        : 'Simulated press; no binding matched.';
      result.className = 'run-result ok';
      result.hidden = false;
    });
  });
  const editBtn = el('button', { text: 'Edit', attrs: { type: 'button' },
    on: { click: () => openBindingEditor(b, null) } });
  const delBtn = el('button', { cls: 'danger', text: 'Delete', attrs: { type: 'button' } });
  delBtn.addEventListener('click', () => {
    if (!confirm(`Delete binding "${b.name}"?`)) return;
    guarded(delBtn, null, async () => {
      await api('DELETE', '/api/bindings/' + encodeURIComponent(b.id));
      await refresh();
      toast('Binding deleted.');
    });
  });

  const card = el('div', { cls: 'card' + (b.enabled ? '' : ' disabled') },
    el('div', { cls: 'card-title' },
      el('h3', { text: b.name }),
      el('span', { cls: 'toggle' }, toggle, el('label', { text: 'Enabled', attrs: { for: 'en-' + b.id } }))),
    el('p', { cls: 'meta', text: triggerSummary(b.trigger) }),
    el('p', { cls: 'summary', text: actionSummary(b.action) }),
    el('div', { cls: 'actions' }, runBtn, simBtn, editBtn, delBtn),
    result);
  return card;
}

$('add-binding').addEventListener('click', () => openBindingEditor(null, null));

// ---- Binding editor -----------------------------------------------------------------------------

const OTHER = '__other__';
const INPUTS = [['button:1', 'Button 1'], ['button:2', 'Button 2'], ['button:3', 'Button 3'],
  ['button:4', 'Button 4'], ['wheel:left', 'Wheel left'], ['wheel:right', 'Wheel right']];
const CHANNELS = [['', 'Any'], ['0', '0'], ['1', '1'], ['2', '2'], ['3', '3']];

// Editor session: the binding being edited plus the SwitchBot lists loaded so far.
const ed = {
  id: null, devices: null, scenes: null, commands: [], storedAction: null,
  cmdGen: 0, listGen: 0,
};

function fillSelect(select, pairs, value) {
  select.replaceChildren(...pairs.map(([v, l]) => option(v, l)));
  // A stored value that is not one of the standard choices must still be shown and kept.
  if (value !== undefined && value !== null && !pairs.some(([v]) => v === value)) {
    select.append(option(value, value));
  }
  if (value !== undefined && value !== null) select.value = value;
}

function fillEvents(input, selected) {
  const events = input.startsWith('wheel:')
    ? state.choices.wheel_events
    : state.choices.button_events;
  fillSelect($('b-event'), events.map((e) => [e.value, e.label]),
    events.some((e) => e.value === selected) ? selected : events[0].value);
}

function showActionSection() {
  const t = $('b-type').value;
  $('sec-http').hidden = t !== 'http';
  $('sec-sbcmd').hidden = t !== 'switchbot_command';
  $('sec-sbscene').hidden = t !== 'switchbot_scene';
  setError($('sb-error'), '');
  if (t === 'switchbot_command' && ed.devices === null) loadDevices(false);
  if (t === 'switchbot_scene' && ed.scenes === null) loadScenes(false);
}

function headersToText(h) {
  return Object.entries(h || {}).map(([k, v]) => `${k}: ${v}`).join('\n');
}

function openBindingEditor(binding, preset) {
  const isNew = !binding;
  ed.id = binding ? binding.id : null;
  ed.devices = null;
  ed.scenes = null;
  ed.commands = [];
  ed.storedAction = binding ? binding.action : null;
  ed.listGen++;
  ed.cmdGen++;
  const src = binding || {
    name: '', enabled: true, cooldown_ms: 700,
    trigger: preset || { button: state.buttons.length ? state.buttons[0].id : '', input: 'button:1',
      event: 'press', channel: null },
    action: { type: 'http', method: 'GET', url: '' },
  };
  const a = src.action;

  $('binding-title').textContent = isNew ? 'Add binding' : 'Edit binding';
  setError($('binding-error'), '');
  setError($('sb-error'), '');
  $('b-name').value = src.name;
  $('b-enabled').checked = src.enabled;

  const buttons = state.buttons.map((b) => [b.id, b.name]);
  $('b-nobuttons').hidden = buttons.length > 0;
  $('binding-save').disabled = buttons.length === 0;
  fillSelect($('b-button'), buttons, src.trigger.button);
  if (!buttons.length) $('b-button').replaceChildren();
  fillSelect($('b-input'), INPUTS, src.trigger.input);
  fillEvents(src.trigger.input, src.trigger.event);
  const ch = src.trigger.channel === null || src.trigger.channel === undefined ? '' : String(src.trigger.channel);
  fillSelect($('b-channel'), CHANNELS, ch);
  $('b-cooldown').value = src.cooldown_ms;

  $('b-type').value = a.type;
  fillSelect($('h-method'), state.choices.http_methods.map((m) => [m, m]), a.method || 'GET');
  $('h-url').value = a.type === 'http' ? a.url : '';
  $('h-headers').value = a.type === 'http' ? headersToText(a.headers) : '';
  $('h-body').value = a.type === 'http' ? (a.body || '') : '';
  $('h-timeout').value = a.type === 'http' && a.timeout_s ? a.timeout_s : '';

  let param = 'default';
  let isJson = false;
  if (a.type === 'switchbot_command') {
    if (typeof a.parameter === 'string') param = a.parameter;
    else { param = JSON.stringify(a.parameter); isJson = true; }
  }
  $('c-param').value = param;
  $('c-json').checked = isJson;
  $('c-ctype').value = a.type === 'switchbot_command' ? (a.command_type || 'command') : 'command';
  // Clear leftovers from a previous editing session so the stored choice is what gets selected.
  $('c-device').replaceChildren();
  $('s-scene').replaceChildren();
  $('c-command-other').value = '';
  renderDevices();
  renderCommands(a.type === 'switchbot_command' ? a.command : null);
  renderScenes();

  $('binding-dialog').showModal();
  showActionSection();
  $('b-name').focus();
  if (a.type === 'switchbot_command') loadCommands(a.device_id, a.command);
}

$('b-input').addEventListener('change', () => fillEvents($('b-input').value, $('b-event').value));
$('b-type').addEventListener('change', showActionSection);
$('binding-cancel').addEventListener('click', () => $('binding-dialog').close());

// Adds the hint "Open Settings" when the failure is about missing SwitchBot credentials.
function switchbotMessage(e) {
  let m = e.message;
  if (/credential|token|secret/i.test(m)) m += ' Set them in Settings, SwitchBot credentials.';
  return m;
}

// -- device list

function renderDevices() {
  const sel = $('c-device');
  const stored = ed.storedAction && ed.storedAction.type === 'switchbot_command' ? ed.storedAction : null;
  const wanted = sel.value || (stored ? stored.device_id : '');
  const pairs = [['', 'Select a device...']];
  for (const d of ed.devices || []) pairs.push([d.id, d.name + (d.type ? ` (${d.type})` : '')]);
  sel.replaceChildren(...pairs.map(([v, l]) => option(v, l)));
  // The saved device stays selectable even if the list is not loaded or no longer contains it.
  if (wanted && !pairs.some(([v]) => v === wanted)) {
    const name = stored && stored.device_id === wanted && stored.device_name ? stored.device_name : wanted;
    sel.append(option(wanted, `${name} (saved)`));
  }
  sel.value = wanted;
}

async function loadDevices(refreshList) {
  const gen = ++ed.listGen;
  setError($('sb-error'), '');
  const btn = $('c-refresh');
  try {
    await withBusy(btn, async () => {
      const r = await api('GET', '/api/switchbot/devices' + (refreshList ? '?refresh=1' : ''));
      if (gen !== ed.listGen) return;
      ed.devices = r.devices;
      renderDevices();
    });
  } catch (e) {
    if (gen === ed.listGen && !e.sessionEnded) {
      ed.devices = ed.devices || [];
      setError($('sb-error'), switchbotMessage(e));
    }
  }
}
$('c-refresh').addEventListener('click', () => loadDevices(true));

// -- command list

function currentCommandObj() {
  return ed.commands.find((c) => c.command === $('c-command').value) || null;
}

function updateCommandHelp() {
  const other = $('c-command').value === OTHER;
  $('c-command-other').hidden = !other;
  const help = $('c-help');
  const c = currentCommandObj();
  const parts = [];
  if (c && c.description) parts.push(c.description);
  if (c && c.parameter) parts.push('Parameter: ' + c.parameter);
  help.textContent = parts.join(' ');
  help.hidden = parts.length === 0;
}

function renderCommands(selected) {
  const sel = $('c-command');
  sel.replaceChildren(...ed.commands.map((c) => option(c.command, c.command)), option(OTHER, 'Other (type it)'));
  const known = selected && ed.commands.some((c) => c.command === selected);
  if (selected && !known) {
    sel.value = OTHER;
    $('c-command-other').value = selected;
  } else if (known) {
    sel.value = selected;
  } else {
    sel.value = ed.commands.length ? ed.commands[0].command : OTHER;
  }
  updateCommandHelp();
}

async function loadCommands(deviceId, selected) {
  const gen = ++ed.cmdGen;
  ed.commands = [];
  const notes = $('c-notes');
  notes.replaceChildren();
  notes.hidden = true;
  if (!deviceId) {
    renderCommands(selected);
    return;
  }
  try {
    const r = await api('GET', '/api/switchbot/commands?device_id=' + encodeURIComponent(deviceId));
    if (gen !== ed.cmdGen) return;
    ed.commands = r.commands || [];
    for (const n of r.notes || []) notes.append(el('li', { text: n }));
    notes.hidden = !(r.notes || []).length;
    setError($('sb-error'), '');
  } catch (e) {
    if (gen !== ed.cmdGen || e.sessionEnded) return;
    setError($('sb-error'), switchbotMessage(e));
  }
  renderCommands(selected);
}

$('c-device').addEventListener('change', () => loadCommands($('c-device').value, null));
$('c-command').addEventListener('change', updateCommandHelp);

// -- scene list

function renderScenes() {
  const sel = $('s-scene');
  const stored = ed.storedAction && ed.storedAction.type === 'switchbot_scene' ? ed.storedAction : null;
  const wanted = sel.value || (stored ? stored.scene_id : '');
  const pairs = [['', 'Select a scene...']];
  for (const s of ed.scenes || []) pairs.push([s.id, s.name]);
  sel.replaceChildren(...pairs.map(([v, l]) => option(v, l)));
  if (wanted && !pairs.some(([v]) => v === wanted)) {
    const name = stored && stored.scene_id === wanted && stored.scene_name ? stored.scene_name : wanted;
    sel.append(option(wanted, `${name} (saved)`));
  }
  sel.value = wanted;
}

async function loadScenes(refreshList) {
  const gen = ++ed.listGen;
  setError($('sb-error'), '');
  try {
    await withBusy($('s-refresh'), async () => {
      const r = await api('GET', '/api/switchbot/scenes' + (refreshList ? '?refresh=1' : ''));
      if (gen !== ed.listGen) return;
      ed.scenes = r.scenes;
      renderScenes();
    });
  } catch (e) {
    if (gen === ed.listGen && !e.sessionEnded) {
      ed.scenes = ed.scenes || [];
      setError($('sb-error'), switchbotMessage(e));
    }
  }
}
$('s-refresh').addEventListener('click', () => loadScenes(true));

// -- saving

// Parses "Name: value" lines into an object; throws Error with the offending line on bad input.
function parseHeaders(text) {
  const headers = {};
  text.split('\n').forEach((raw, i) => {
    const line = raw.trim();
    if (!line) return;
    const idx = line.indexOf(':');
    const name = idx > 0 ? line.slice(0, idx).trim() : '';
    if (!name || /\s/.test(name)) throw new Error(`Header line ${i + 1} must look like "Name: value".`);
    headers[name] = line.slice(idx + 1).trim();
  });
  return headers;
}

// Display name for a selected id: the loaded list wins, then the name stored in the binding.
function displayName(list, id, storedId, storedName) {
  const found = (list || []).find((x) => x.id === id);
  if (found) return found.name;
  return id === storedId && storedName ? storedName : id;
}

// Collects the form into an API payload, throwing Error with a user-facing message if invalid.
function collectBinding() {
  const name = $('b-name').value.trim();
  if (!name) throw new Error('Enter a name for the binding.');
  const button = $('b-button').value;
  if (!button) throw new Error('Choose a button. If there are none, add one first.');
  const cooldownText = $('b-cooldown').value.trim();
  const cooldown = cooldownText === '' ? 700 : Number(cooldownText);
  if (!Number.isInteger(cooldown) || cooldown < 0 || cooldown > 60000) {
    throw new Error('Cooldown must be a whole number of milliseconds from 0 to 60000.');
  }
  const channel = $('b-channel').value === '' ? null : Number($('b-channel').value);

  let action;
  const type = $('b-type').value;
  const stored = ed.storedAction || {};
  if (type === 'http') {
    const url = $('h-url').value.trim();
    if (!/^https?:\/\/\S+$/i.test(url)) throw new Error('The URL must start with http:// or https://.');
    action = { type, method: $('h-method').value, url };
    const headers = parseHeaders($('h-headers').value);
    if (Object.keys(headers).length) action.headers = headers;
    const body = $('h-body').value;
    if (body) action.body = body;
    const t = $('h-timeout').value.trim();
    if (t !== '') {
      const n = Number(t);
      if (!(n >= 1 && n <= 60)) throw new Error('The timeout must be between 1 and 60 seconds.');
      action.timeout_s = n;
    }
  } else if (type === 'switchbot_command') {
    const deviceId = $('c-device').value;
    if (!deviceId) throw new Error('Choose a SwitchBot device.');
    const command = ($('c-command').value === OTHER ? $('c-command-other').value : $('c-command').value).trim();
    if (!command) throw new Error('Choose or type a command.');
    let parameter = $('c-param').value.trim() || 'default';
    if ($('c-json').checked) {
      try {
        parameter = JSON.parse(parameter);
      } catch (e) {
        throw new Error('The parameter is not valid JSON: ' + e.message);
      }
    }
    action = { type, device_id: deviceId, device_name: displayName(ed.devices, deviceId, stored.device_id, stored.device_name), command,
      parameter, command_type: $('c-ctype').value };
  } else {
    const sceneId = $('s-scene').value;
    if (!sceneId) throw new Error('Choose a SwitchBot scene.');
    action = { type, scene_id: sceneId, scene_name: displayName(ed.scenes, sceneId, stored.scene_id, stored.scene_name) };
  }
  return {
    name, enabled: $('b-enabled').checked, cooldown_ms: cooldown,
    trigger: { button, channel, input: $('b-input').value, event: $('b-event').value },
    action,
  };
}

$('binding-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const err = $('binding-error');
  setError(err, '');
  let payload;
  try {
    payload = collectBinding();
  } catch (e) {
    setError(err, e.message);
    return;
  }
  guarded($('binding-save'), err, async () => {
    if (ed.id) await api('PUT', '/api/bindings/' + encodeURIComponent(ed.id), payload);
    else await api('POST', '/api/bindings', payload);
    $('binding-dialog').close();
    await refresh();
    toast('Binding saved.');
  });
});

// ---- Log tab ------------------------------------------------------------------------------------

const MAX_LOG = 300;
const KIND_LABELS = { ble: 'BLE', fire: 'Fired', action: 'Action', error: 'Error', info: 'Info' };
const logs = { entries: [], last: 0, gen: 0, timer: null, running: false };

function pad2(n) { return String(n).padStart(2, '0'); }

function formatTime(unixSeconds) {
  const d = new Date(unixSeconds * 1000);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

function renderLog() {
  const list = $('log-list');
  list.replaceChildren();
  if (!logs.entries.length) {
    list.append(el('li', { text: 'No events yet.' }));
    return;
  }
  for (const e of logs.entries) {
    const kind = KIND_LABELS[e.kind] ? e.kind : 'info';
    list.append(el('li', { cls: 'kind-' + kind },
      el('span', { cls: 'time', text: formatTime(e.time) }),
      el('span', { cls: 'kind', text: KIND_LABELS[kind] }),
      el('span', { text: e.text })));
  }
}

function ingestLog(data) {
  // A smaller last_seq means the server restarted; start over from its beginning.
  if (data.last_seq < logs.last) {
    logs.entries = [];
    logs.last = 0;
    renderLog();
    return;
  }
  logs.last = data.last_seq;
  if (!data.entries.length) return;
  logs.entries = data.entries.slice().reverse().concat(logs.entries).slice(0, MAX_LOG);
  renderLog();
}

function startLogPolling() {
  if (logs.running) return;
  logs.running = true;
  const gen = ++logs.gen;
  const tick = async () => {
    if (gen !== logs.gen) return;
    try {
      const d = await api('GET', '/api/log?since=' + logs.last);
      if (gen !== logs.gen) return;
      setError($('log-error'), '');
      ingestLog(d);
    } catch (e) {
      if (gen !== logs.gen || e.sessionEnded) return;
      setError($('log-error'), e.message);
    }
    if (gen === logs.gen) logs.timer = setTimeout(tick, 1000);
  };
  tick();
}

function stopLogPolling() {
  logs.running = false;
  logs.gen++;
  clearTimeout(logs.timer);
}

// Polls only while the Log tab is on screen.
function syncLogPolling() {
  if (currentTab === 'log' && !document.hidden && !$('app-view').hidden) startLogPolling();
  else stopLogPolling();
}
document.addEventListener('visibilitychange', syncLogPolling);

$('log-clear').addEventListener('click', () => {
  logs.entries = [];
  renderLog();
});

// ---- Settings tab -------------------------------------------------------------------------------

// How long a Settings button stays disabled after it succeeded (see withBusy).
const SETTINGS_COOLDOWN_MS = 5000;

function renderSettings() {
  $('set-timeout').value = state.settings.http_timeout_s;
  const status = $('cred-status');
  status.textContent = state.switchbot.configured ? 'Credentials are set.' : 'Credentials are not set.';
}

$('settings-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const n = Number($('set-timeout').value);
  if (!($('set-timeout').value.trim() !== '' && n >= 1 && n <= 60)) {
    return setError($('page-error'), 'The timeout must be between 1 and 60 seconds.');
  }
  guarded(ev.target.querySelector('button[type=submit]'), null, async () => {
    await api('PUT', '/api/settings', { http_timeout_s: n });
    await refresh();
    toast('Settings saved.');
  }, SETTINGS_COOLDOWN_MS);
});

$('cred-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const token = $('cred-token').value.trim();
  const secret = $('cred-secret').value.trim();
  if (!token && !secret) return setError($('page-error'), 'Enter a token and/or a secret.');
  guarded(ev.target.querySelector('button[type=submit]'), null, async () => {
    await api('PUT', '/api/switchbot/credentials', { token, secret });
    $('cred-token').value = '';
    $('cred-secret').value = '';
    await refresh();
    toast('Credentials saved.');
  }, SETTINGS_COOLDOWN_MS);
});

$('cred-remove').addEventListener('click', (ev) => {
  if (!confirm('Remove the stored SwitchBot token and secret?')) return;
  guarded(ev.currentTarget, null, async () => {
    await api('DELETE', '/api/switchbot/credentials');
    await refresh();
    toast('Credentials removed.');
  }, SETTINGS_COOLDOWN_MS);
});

$('password-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const current = $('pw-current').value;
  const next = $('pw-new').value;
  const err = $('page-error');
  if (!current) return setError(err, 'Enter the current password.');
  if (next.length < 8) return setError(err, 'The new password must be at least 8 characters.');
  if (next !== $('pw-repeat').value) return setError(err, 'The new passwords do not match.');
  guarded(ev.target.querySelector('button[type=submit]'), null, async () => {
    await api('POST', '/api/auth/password', { current, new: next });
    for (const id of ['pw-current', 'pw-new', 'pw-repeat']) $(id).value = '';
    toast('Password changed.');
  }, SETTINGS_COOLDOWN_MS);
});

// The export link is a plain download; the checkbox only decides whether credentials are included.
$('export-creds').addEventListener('change', () => {
  $('export-link').setAttribute('href', '/api/export' + ($('export-creds').checked ? '?credentials=1' : ''));
});

// A download gives no feedback while the server prepares it, so the link looks pressed (and ignores further
// clicks) for a few seconds to stop impatient repeat clicks from starting several downloads.
$('export-link').addEventListener('click', (ev) => {
  const link = ev.currentTarget;
  if (link.getAttribute('aria-disabled') === 'true') {
    ev.preventDefault();
    return;
  }
  link.setAttribute('aria-disabled', 'true');
  setTimeout(() => link.removeAttribute('aria-disabled'), SETTINGS_COOLDOWN_MS);
});

$('import-go').addEventListener('click', (ev) => {
  const err = $('import-error');
  const res = $('import-result');
  setError(err, '');
  res.hidden = true;
  const file = $('import-file').files[0];
  if (!file) return setError(err, 'Choose a .json file first.');
  const mode = $('import-mode').value;
  if (mode === 'replace' && !confirm('Replace all buttons, bindings and settings with the contents of this file?')) return;
  guarded(ev.currentTarget, err, async () => {
    let config;
    try {
      config = JSON.parse(await file.text());
    } catch (e) {
      throw new Error('The file is not valid JSON.');
    }
    const r = await api('POST', '/api/import', { config, mode });
    await refresh();
    $('import-file').value = '';
    res.textContent = `Imported. Now ${r.buttons} button(s) and ${r.bindings} binding(s).`;
    res.hidden = false;
  }, SETTINGS_COOLDOWN_MS);
});

// ---- start --------------------------------------------------------------------------------------

init();
