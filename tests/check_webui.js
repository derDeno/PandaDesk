// Run with node tests/check_webui.js; exercise the page's actual script without hardware.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map(([, id]) => [
  `#${id}`, {textContent: '', hidden: true, value: '', disabled: false, checked: false, files: [],
    replaceChildren(...options) { this.options = options; this.value = options[0]?.value || ''; },
    showModal() { this.open = true; }, close() { this.open = false; },
    setAttribute(name, value) { this[name] = value; },
    removeAttribute(name) { delete this[name]; }}
]));
assert.equal(elements.size, [...html.matchAll(/id="([^"]+)"/g)].length, 'IDs must be unique');
const element = selector => {
  assert(elements.has(selector), `Missing element ${selector}`);
  return elements.get(selector);
};
let countdownAnimation;
let toastInteracting = false;
element('#settings-toast').matches = () => toastInteracting;
element('#toast-countdown-fill').animate = (keyframes, options) => {
  countdownAnimation = {keyframes, options, cancelled: false, playState: 'running', currentTime: 0,
    effect: {getTiming() { return options; }},
    pause() { this.playState = 'paused'; }, play() { this.playState = 'running'; },
    cancel() { this.cancelled = true; this.playState = 'idle'; }};
  return countdownAnimation;
};
for (const [, id, content] of html.matchAll(/<select id="([^"]+)"[^>]*>([\s\S]*?)<\/select>/g)) {
  element(`#${id}`).options = [...content.matchAll(/<option value="([^"]*)"( disabled)?>([^<]*)<\/option>/g)]
    .map(([, value, disabled, text]) => ({value, text, disabled: Boolean(disabled)}));
}
const movementButtons = ['up', 'stop', 'down'].map(direction => ({dataset: {direction}, disabled: true}));
const pageLinks = [...html.matchAll(/data-page="([^"]+)"/g)].map(([, page]) => ({dataset: {page},
  setAttribute(name, value) { this[name] = value; }, removeAttribute(name) { delete this[name]; }}));
assert.deepEqual(pageLinks.map(link => link.dataset.page), ['home', 'info', 'settings']);
const homeMarkup = html.match(/<main id="home-page">([\s\S]*?)<\/main>/)[1];
const infoMarkup = html.match(/<main id="info-page" hidden>([\s\S]*?)<\/main>/)[1];
assert(!/id="(?:network-ssid|status-firmware-version|mqtt-status-details)"/.test(homeMarkup), 'Home keeps desk information and controls');
for (const id of ['network-ssid', 'status-firmware-version', 'mqtt-status-details', 'logs-output']) {
  assert(infoMarkup.includes(`id="${id}"`), `${id} belongs on Info`);
}
assert.match(infoMarkup, /id="info-details"[^>]* open>/, 'Device information opens by default');
assert(!/id="logs-details"[^>]* open>/.test(infoMarkup), 'Logs stay collapsed until requested');
for (const id of ['info-project', 'info-sdk', 'info-build', 'info-sha256', 'info-partition',
  'info-firmware-size', 'info-webui-size', 'info-heap-largest', 'info-tasks', 'info-log-size']) {
  assert(!infoMarkup.includes(`id="${id}"`), `${id} was removed from Info`);
}
assert.match(infoMarkup, /id="info-refresh"[^>]*><svg[^>]*aria-hidden="true"[\s\S]*?<\/svg>Refresh<\/button>/);
assert(!script.includes('Snapshot at'), 'Successful refreshes need no snapshot message');
assert(!html.includes('Live · updates every 2 seconds'), 'Log activity uses the toolbar badge');
assert.match(infoMarkup, /id="logs-status"[\s\S]*?id="logs-status-label"[\s\S]*?id="logs-download"/, 'The status badge sits before Download logs');
let timerId = 0;
const timers = new Map();
let responseOk = true;
let lastRequest;
let lastUpload;
let uploadCount = 0;
let reloadCount = 0;
class UploadRequest {
  constructor() { this.upload = {}; lastUpload = this; ++uploadCount; }
  open(method, url) { this.method = method; this.url = url; }
  setRequestHeader(name, value) { this.headers = {[name]: value}; }
  send(file) { this.file = file; }
}
const context = vm.createContext({
  AbortController,
  Option: class { constructor(text, value) { this.text = text; this.value = value; this.disabled = false; } },
  document: {querySelector: element, querySelectorAll(selector) {
    if (selector === '[data-direction]') return movementButtons;
    if (selector === '[data-page]') return pageLinks;
    assert.equal(selector, '.control-tooltip');
    return [];
  }},
  location: {hostname: '192.168.1.42', protocol: 'http:', reload() { ++reloadCount; }},
  XMLHttpRequest: UploadRequest,
  window: {scrollTo() { assert.fail('Notifications must not scroll the page'); }},
  setTimeout(callback, delay) { timers.set(++timerId, {callback, delay}); return timerId; },
  clearTimeout(id) { timers.delete(id); },
  fetch: async (url, options) => {
    lastRequest = {url, body: JSON.parse(options.body)};
    return {ok: responseOk, json: async () => ({})};
  }
});
// Omit automatic event binding/polling; call the same handlers directly.
vm.runInContext(script.slice(0, script.lastIndexOf("    bindMovementControls();")), context);
const run = code => vm.runInContext(code, context);
const state = {
  firmware_version: '0.1.0', filesystem_version: '0.1.0', uptime: 3661,
  network: {mode: 'station', connected: true, mdns_available: true, ssid: '<Wi-Fi & "test">',
    hostname: 'pandadesk-38b4', rssi: -48, mac: '90:64:9B:BB:38:B4', ip: '192.168.1.42'}
};
context.state = state;
run('showDeviceStatus(state)');
assert.equal(element('#network-ssid').textContent, state.network.ssid);
assert.equal(element('#network-rssi').textContent, 'Excellent (-48 dBm)');
for (const [rssi, label] of [[-50, 'Excellent'], [-51, 'Good'], [-60, 'Good'], [-61, 'Fair'],
  [-70, 'Fair'], [-71, 'Weak'], [-80, 'Weak'], [-81, 'Very weak']]) {
  context.state = {...state, network: {...state.network, rssi}};
  run('showDeviceStatus(state)');
  assert.equal(element('#network-rssi').textContent, `${label} (${rssi} dBm)`);
}
context.state = state;
assert.equal(element('#device-uptime').textContent, '1h 1m 1s');
assert.equal(element('#mdns-notice').hidden, false);
assert.equal(element('#mdns-link').href, 'http://pandadesk-38b4.local/');
for (const host of ['pandadesk.local', 'pandadesk-38b4.local', 'localhost']) {
  context.location.hostname = host;
  run('showDeviceStatus(state)');
  assert.equal(element('#mdns-notice').hidden, true);
}
context.location.hostname = '[fe80::1234]';
run('showDeviceStatus(state)');
assert.equal(element('#mdns-notice').hidden, false);
for (const overrides of [
  {connected: false}, {mdns_available: false},
  {connected: false, mode: 'ap', rssi: null}
]) {
  context.state = {...state, network: {...state.network, ...overrides}};
  run('showDeviceStatus(state)');
  assert.equal(element('#mdns-notice').hidden, true);
}
assert.equal(element('#network-mode').textContent, 'Setup access point');
assert.equal(element('#network-rssi').textContent, '—');
run("showSettingsToast('First save')");
const previousTimer = timerId;
const previousAnimation = countdownAnimation;
run("showSettingsToast('Second save')");
assert(!timers.has(previousTimer), 'A new toast must cancel the previous expiry');
assert(previousAnimation.cancelled, 'A replacement must cancel the previous countdown');
assert.notEqual(countdownAnimation, previousAnimation, 'Each toast starts a new countdown');
assert.equal(element('#toast-countdown').hidden, false);
assert.equal(countdownAnimation.options.duration, timers.get(timerId).delay, 'Ring and expiry must share one duration');
assert.equal(countdownAnimation.options.easing, 'linear');
assert.equal(countdownAnimation.keyframes[0].strokeDashoffset, '100', 'Countdown starts empty');
assert.equal(countdownAnimation.keyframes[1].strokeDashoffset, '0', 'Countdown ends full');
assert.equal(element('#toast-message').textContent, 'Second save');
assert.equal(element('#settings-toast').className, 'alert alert-success');
assert.equal(timers.get(timerId).delay, 5000);
timers.get(timerId).callback();
assert.equal(element('#settings-toast').hidden, true);
assert.equal(element('#toast-message').textContent, '');
assert.equal(element('#toast-countdown').hidden, true);
assert(countdownAnimation.cancelled, 'Expiry cleans up the animation');
run("showSettingsToast('Pause while reading')");
countdownAnimation.currentTime = 1600;
toastInteracting = true;
run('pauseSettingsToast()');
assert.equal(timers.size, 0, 'Hover must stop automatic dismissal');
assert.equal(countdownAnimation.playState, 'paused', 'Hover must also stop the ring');
run('resumeSettingsToast()');
assert.equal(timers.size, 0, 'Leaving focus must not resume while still hovered, or vice versa');
toastInteracting = false;
run('resumeSettingsToast()');
assert.equal(countdownAnimation.playState, 'running');
assert.equal(timers.get(timerId).delay, 3400, 'Resume must keep the remaining time');
countdownAnimation.currentTime = 2200;
run('pauseSettingsToast(); resumeSettingsToast()');
assert.equal(timers.size, 1, 'Repeated hovering must leave only one expiry');
assert.equal(timers.get(timerId).delay, 2800);
toastInteracting = true;
run("showSettingsToast('Replacement while hovered')");
assert.equal(timers.size, 0, 'A replacement under the pointer must start paused');
assert.equal(countdownAnimation.playState, 'paused');
assert.equal(countdownAnimation.currentTime, 0);
run('clearSettingsToast(); resumeSettingsToast()');
assert.equal(timers.size, 0, 'Dismissed toasts must not resume');
toastInteracting = false;
run("showSettingsToast('Saved')");
const replacedTimer = timerId;
run("showSettingsToast('Could not save', 'error')");
assert.equal(element('#toast-countdown').hidden, true, 'Persistent errors must have no countdown');
assert(countdownAnimation.cancelled);
assert(!timers.has(replacedTimer), 'An error must cancel the previous success timeout');
assert.equal(timers.size, 0, 'Errors stay visible until dismissed');
assert.equal(element('#settings-toast').hidden, false);
run('pauseSettingsToast(); resumeSettingsToast()');
assert.equal(timers.size, 0, 'Persistent errors must remain untimed');
run('clearSettingsToast()');
assert.equal(element('#settings-toast').hidden, true);
assert.equal(element('#toast-message').textContent, '');
assert(!html.includes('id="command-message"'), 'Command feedback uses the shared toast');

(async () => {
  const settingsFetch = context.fetch;
  const listen = target => {
    const listeners = new Map();
    target.addEventListener = (type, callback) => listeners.set(type, callback);
    return (type, overrides = {}) => {
      let prevented = false;
      listeners.get(type)?.({button: 0, pointerId: 1, detail: 1, repeat: false,
        preventDefault() { prevented = true; }, ...overrides});
      return prevented;
    };
  };
  const buttonEvents = movementButtons.map(button => {
    button.disabled = false;
    button.setPointerCapture = id => { button.capturedPointer = id; };
    return listen(button);
  });
  const [up, stop, down] = buttonEvents;
  const windowEvent = listen(context.window);
  const documentEvent = listen(context.document);
  run('bindMovementControls()');
  const moves = [];
  context.fetch = async (url, options) => {
    assert.equal(url, '/api/v1/commands/move');
    moves.push(JSON.parse(options.body));
    if (moves.at(-1).direction === 'stop') assert.equal(options.keepalive, true);
    return {ok: true};
  };
  const movementFetch = context.fetch;
  const settleMoves = async () => {
    await new Promise(resolve => setImmediate(resolve));
    await run('moveRequests');
    await new Promise(resolve => setImmediate(resolve));
  };
  const fireTimer = delay => {
    const entry = [...timers].find(([, timer]) => timer.delay === delay);
    assert(entry, `Missing ${delay} ms timer`);
    timers.delete(entry[0]);
    entry[1].callback();
  };
  const directions = () => moves.map(move => move.direction);
  const resetMoves = () => { moves.length = 0; run('clearSettingsToast()'); timers.clear(); };

  up('pointerdown');
  up('pointerup');
  up('click');
  await settleMoves();
  assert.deepEqual(directions(), ['up'], 'A quick click remains one nudge, without a duplicate click command');
  assert.equal(moves[0].expires_ms, 1000);
  assert.equal(timers.size, 1, 'Only the success toast timer remains after a tap');
  resetMoves();

  up('pointerdown');
  await settleMoves();
  assert.equal(element('#settings-toast').hidden, true, 'An accepted move stays quiet until release');
  up('pointerup');
  await settleMoves();
  assert.equal(element('#settings-toast').hidden, false, 'A tap shows confirmation after release');
  assert.deepEqual(directions(), ['up']);
  resetMoves();

  for (const [events, direction, pointerType] of [[up, 'up', 'mouse'], [down, 'down', 'touch'], [up, 'up', 'pen']]) {
    events('pointerdown', {pointerType});
    await settleMoves();
    assert.equal(element('#settings-toast').hidden, true, 'Pressing must not show a toast');
    fireTimer(350);
    await settleMoves();
    fireTimer(250);
    await settleMoves();
    assert.deepEqual(directions(), [direction, direction, direction]);
    assert(moves.every(move => move.expires_ms === 1000));
    assert.equal(new Set(moves.map(move => move.request_id)).size, 3);
    assert.equal(element('#settings-toast').hidden, true, 'Holding and renewing must not show a toast');
    assert.equal(timers.size, 1, 'Only the renewal timer runs while holding');
    events('pointerup', {pointerType});
    events('lostpointercapture');
    events('click');
    await settleMoves();
    assert.deepEqual(directions(), [direction, direction, direction, 'stop']);
    assert.equal(moves.at(-1).expires_ms, undefined);
    assert.equal(element('#settings-toast').hidden, false, 'A hold shows confirmation after release and Stop');
    assert.equal(timers.size, 1, 'Release cancels renewal');
    resetMoves();
  }

  for (const key of [' ', 'Enter']) {
    assert(up('keydown', {key}));
    up('keydown', {key, repeat: true});
    await settleMoves();
    fireTimer(350);
    await settleMoves();
    assert(up('keyup', {key}));
    await settleMoves();
    assert.deepEqual(directions(), ['up', 'up', 'stop'], 'Keyboard holds ignore autorepeat');
    resetMoves();
  }
  up('keydown', {key: ' '});
  up('keyup', {key: ' '});
  await settleMoves();
  assert.deepEqual(directions(), ['up'], 'A keyboard tap remains one nudge');
  resetMoves();
  up('click', {detail: 0});
  await settleMoves();
  assert.deepEqual(directions(), ['up'], 'Assistive activation remains available');
  resetMoves();

  for (const cancel of [
    () => up('pointercancel'), () => up('lostpointercapture'), () => up('blur'),
    () => windowEvent('blur'), () => windowEvent('pagehide'),
    () => { context.document.hidden = true; documentEvent('visibilitychange'); context.document.hidden = false; },
    () => { context.location.hash = '#settings'; run('showPage()'); context.location.hash = '#home'; },
    () => run('deskReady = false; updateMovementControls()'),
    () => stop('click')
  ]) {
    movementButtons.forEach(button => { button.disabled = false; });
    up('pointerdown');
    await settleMoves();
    fireTimer(350);
    await settleMoves();
    cancel();
    await settleMoves();
    assert.deepEqual(directions(), ['up', 'up', 'stop']);
    assert(![...timers.values()].some(timer => [250, 350].includes(timer.delay)), 'Cancellation clears renewal');
    resetMoves();
  }

  movementButtons.forEach(button => { button.disabled = false; });
  up('pointerdown', {button: 2});
  movementButtons[0].disabled = true;
  up('pointerdown');
  up('click', {detail: 0});
  await settleMoves();
  assert.deepEqual(moves, [], 'Disabled controls and secondary clicks never move');
  movementButtons[0].disabled = false;

  let finishMove;
  context.fetch = (url, options) => {
    const body = JSON.parse(options.body);
    moves.push(body);
    return body.direction === 'stop' ? Promise.resolve({ok: true})
      : new Promise(resolve => { finishMove = resolve; });
  };
  up('pointerdown');
  await new Promise(resolve => setImmediate(resolve));
  fireTimer(350);
  up('pointerup');
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(directions(), ['up'], 'Stop waits for the in-flight move');
  finishMove({ok: true});
  await settleMoves();
  assert.deepEqual(directions(), ['up', 'stop'], 'Release discards queued renewal before sending Stop');
  resetMoves();

  context.fetch = async (url, options) => {
    const result = await movementFetch(url, options);
    return moves.length === 2 ? {ok: false, status: 409, json: async () => ({error: 'child_locked'})} : result;
  };
  up('pointerdown');
  await settleMoves();
  fireTimer(350);
  await settleMoves();
  assert.deepEqual(directions(), ['up', 'up', 'stop'], 'A rejected renewal stops the hold');
  assert.match(element('#toast-message').textContent, /Child lock/);
  assert.equal(timers.size, 0, 'A rejected renewal never schedules another move');
  resetMoves();
  context.fetch = settingsFetch;
  const info = {target: 'esp32c6', pcb_revision: 'v1.0', cpu_cores: 1, cpu_mhz: 160,
    flash_bytes: 8388608, reset_reason: 3, heap_free_bytes: 204800, heap_min_free_bytes: 102400};
  context.info = info;
  run('showSystemInfo(info)');
  assert.equal(element('#info-target').textContent, 'esp32c6');
  assert.equal(element('#info-revision').textContent, 'v1.0');
  assert.equal(element('#info-cpu').textContent, '160 MHz');
  assert.equal(element('#info-flash').textContent, '8.0 MiB');
  assert.equal(element('#info-reset').textContent, 'Software restart');
  assert.equal(element('#info-heap-free').textContent, '200.0 KiB');
  assert.equal(element('#info-heap-min').textContent, '100.0 KiB');
  run('showSystemInfo({})');
  assert.equal(element('#info-flash').textContent, '—', 'Missing values are not reported as zero');
  assert.equal(element('#info-revision').textContent, '—');
  assert.equal(element('#info-reset').textContent, 'Unknown');
  const infoCalls = [];
  let finishInfo;
  context.fetch = (url, options) => {
    infoCalls.push(url);
    assert.equal(options.cache, 'no-store');
    return url === '/api/v1/info' ? new Promise(resolve => { finishInfo = resolve; })
      : Promise.resolve({ok: true, json: async () => state});
  };
  await run('loadInfo()');
  assert.equal(infoCalls.length, 0, 'Home must not request hardware diagnostics');
  context.location.hash = '#info';
  element('#info-details').open = true;
  run('showPage()');
  assert.equal(element('#home-page').hidden, true);
  assert.equal(element('#info-page').hidden, false);
  assert.equal(pageLinks[1]['aria-current'], 'page');
  assert.equal(element('#info-refresh').disabled, true);
  await run('loadInfo()');
  assert.deepEqual(infoCalls, ['/api/v1/info', '/api/v1/state'], 'Repeated clicks do not overlap information requests');
  finishInfo({ok: true, json: async () => info});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(element('#info-refresh').disabled, false);
  assert.equal(element('#info-status').textContent, '');
  assert.equal(element('#info-status').hidden, true, 'Successful refreshes leave the status message hidden');
  assert.equal(element('#network-ssid').textContent, state.network.ssid);
  assert.equal(element('#device-uptime').textContent, '1h 1m 1s');
  assert.equal(timers.size, 0, 'Information snapshots need no background timer');
  context.fetch = async url => { infoCalls.push(url); return {ok: false}; };
  await run('loadInfo()');
  assert.match(element('#info-status').textContent, /Try Refresh/);
  assert.equal(element('#info-status').hidden, false, 'Refresh failures remain visible');
  assert.equal(element('#info-revision').textContent, info.pcb_revision, 'Failed refresh preserves previous information');
  assert.equal(element('#info-refresh').disabled, false);
  element('#info-details').open = false;
  const previousInfoCalls = infoCalls.length;
  await run('loadInfo()');
  assert.equal(infoCalls.length, previousInfoCalls);
  context.location.hash = '#home';
  run('showPage()');
  let logCalls = 0;
  let logBoot = '123';
  let logCursor = '20';
  let logDropped = '0';
  let logText = '\x1b[32mI (100) PandaDesk: <device & log>\x1b[0m\n';
  context.fetch = async (url, options) => {
    ++logCalls;
    lastRequest = {url, options};
    return {ok: responseOk, headers: {get(name) {
      return {'X-Log-Cursor': logCursor, 'X-Log-Boot': logBoot, 'X-Log-Dropped': logDropped}[name];
    }}, text: async () => logText};
  };
  await run('refreshLogs()');
  assert.equal(logCalls, 0, 'Home must not fetch logs');
  context.location.hash = '#info';
  await run('refreshLogs()');
  assert.equal(logCalls, 0, 'Info must not fetch logs while their card is collapsed');
  element('#logs-details').open = true;
  element('#logs-autoscroll').checked = true;
  element('#logs-output').scrollHeight = 500;
  run('showPage()');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(element('#home-page').hidden, true);
  assert.equal(element('#settings-page').hidden, true);
  assert.equal(element('#info-page').hidden, false);
  assert.equal(pageLinks[1]['aria-current'], 'page');
  assert.equal(element('#logs-output').textContent, 'I (100) PandaDesk: <device & log>\n', 'Logs must remain plain text');
  assert.equal(element('#logs-output').scrollTop, 500);
  assert.equal(lastRequest.url, '/api/v1/logs');
  assert.equal(lastRequest.options.cache, 'no-store');
  assert.equal(element('#logs-status-label').textContent, 'Live');
  assert.equal(element('#logs-status')['data-live'], 'true');
  assert.equal(timers.get(timerId).delay, 2000);
  assert.equal(timers.size, 1, 'Only the next poll remains after a response');
  logText = 'next\n';
  logCursor = '25';
  await run('refreshLogs()');
  assert.equal(lastRequest.url, '/api/v1/logs?since=20&boot=123');
  assert.equal(element('#logs-output').textContent, 'I (100) PandaDesk: <device & log>\nnext\n');
  run('toggleLogsPaused()');
  const pausedCalls = logCalls;
  await run('refreshLogs()');
  assert.equal(logCalls, pausedCalls);
  assert.equal(timers.size, 0, 'Pause stops log polling');
  assert.equal(element('#logs-pause').textContent, 'Resume');
  assert.equal(element('#logs-pause')['aria-pressed'], 'true');
  assert.equal(element('#logs-status-label').textContent, 'Paused');
  assert.equal(element('#logs-status')['data-live'], 'false', 'Paused logs must not pulse as live');
  logDropped = '1';
  logText = 'after pause\n';
  element('#logs-autoscroll').checked = false;
  element('#logs-output').scrollTop = 123;
  run('toggleLogsPaused()');
  await new Promise(resolve => setImmediate(resolve));
  assert.match(element('#logs-output').textContent, /Earlier log output was overwritten/);
  assert.equal(element('#logs-output').scrollTop, 123, 'Auto-scroll can be disabled for reading');
  logBoot = '456';
  logCursor = '10';
  logDropped = '0';
  logText = 'new boot\n';
  await run('refreshLogs()');
  assert.equal(element('#logs-output').textContent, '[Device restarted; previous logs cleared.]\nnew boot\n');
  responseOk = false;
  await run('refreshLogs()');
  assert.equal(element('#logs-status-label').textContent, 'Retrying');
  assert.equal(element('#logs-status')['data-live'], 'false', 'Failed log requests must not show Live');
  assert.match(element('#logs-output').textContent, /new boot/, 'Errors preserve the displayed logs');
  responseOk = true;
  logText = 'x'.repeat(40000);
  await run('refreshLogs()');
  assert.equal(element('#logs-status-label').textContent, 'Live', 'Successful retries restore Live');
  assert.equal(element('#logs-status')['data-live'], 'true');
  assert.equal(element('#logs-output').textContent.length, 32768, 'Browser log history must be bounded');
  element('#logs-details').open = false;
  run('syncLogPolling()');
  const collapsedCalls = logCalls;
  await run('refreshLogs()');
  assert.equal(logCalls, collapsedCalls);
  assert.equal(timers.size, 0, 'Collapsing the Logs card stops polling');
  assert.equal(element('#logs-status-label').textContent, 'Inactive');
  assert.equal(element('#logs-status')['data-live'], 'false');
  element('#logs-details').open = true;
  context.document.hidden = true;
  run('syncLogPolling()');
  const hiddenCalls = logCalls;
  await run('refreshLogs()');
  assert.equal(logCalls, hiddenCalls);
  assert.equal(timers.size, 0, 'Hidden browser tabs must stop log polling');
  context.document.hidden = false;
  let finishLogs;
  let pendingSignal;
  context.fetch = (url, options) => {
    ++logCalls;
    pendingSignal = options.signal;
    return new Promise(resolve => { finishLogs = resolve; });
  };
  const pendingLogs = run('refreshLogs()');
  await run('refreshLogs()');
  assert.equal(logCalls, hiddenCalls + 1, 'Log requests must not overlap');
  context.location.hash = '#settings';
  run('showPage()');
  assert(pendingSignal.aborted, 'Leaving Logs aborts an in-flight request');
  finishLogs({ok: true, headers: {get: name => name === 'X-Log-Boot' ? '456' : '30'}, text: async () => 'stale'});
  await pendingLogs;
  assert.equal(element('#logs-output').textContent.length, 32768, 'Aborted requests must not append stale output');
  assert.equal(timers.size, 0);
  let downloaded;
  let downloadBlob;
  let revoked;
  context.Blob = Blob;
  context.URL = {createObjectURL(blob) { downloadBlob = blob; return 'blob:logs'; }, revokeObjectURL(url) { revoked = url; }};
  context.document.createElement = tag => {
    assert.equal(tag, 'a');
    return {click() { downloaded = {href: this.href, name: this.download}; }};
  };
  run('downloadLogs()');
  assert.equal(await downloadBlob.text(), element('#logs-output').textContent);
  assert.deepEqual(downloaded, {href: 'blob:logs', name: 'pandadesk-logs.txt'});
  timers.get(timerId).callback();
  timers.clear();
  assert.equal(revoked, 'blob:logs');
  context.fetch = settingsFetch;
  context.location.hash = '#home';
  run('setTargetHeightBounds(60, 125)');
  let liveState = {...state, ready: true, profile: 'jiecang_rj12', height: {value: 70, unit: 'cm', quality: 'measured'}, moving: false};
  let liveCapabilities = {profile_configured: true, move: true, stop: true, target_height: true};
  let deskSettings = {};
  context.location.hostname = '192.168.1.42';
  context.fetch = async url => ({ok: responseOk, json: async () => url === '/api/v1/state'
    ? liveState : url === '/api/v1/capabilities' ? liveCapabilities : deskSettings});
  await run('refresh(true)');
  assert.equal(element('#desk-connection').textContent, 'Connected');
  assert.equal(element('#desk-connection')['data-connected'], 'true');
  assert.equal(element('#mqtt-status-message').textContent, 'Not configured yet');
  assert.equal(element('#mqtt-status-message').hidden, false);
  assert.equal(element('#mqtt-status-details').hidden, true, 'Missing MQTT status must hide details');
  liveState = {...liveState, mqtt: {connected: true, host: '<broker & "test">', port: 1883,
    state_topic: 'pandadesk/90649bbb38b4/state', availability_topic: 'pandadesk/90649bbb38b4/availability'}};
  await run('refresh()');
  assert.equal(element('#mqtt-status-message').hidden, true);
  assert.equal(element('#mqtt-status-details').hidden, false);
  assert.equal(element('#mqtt-status-connection').textContent, 'Connected');
  assert.equal(element('#mqtt-status-broker').textContent, '<broker & "test">:1883');
  assert.equal(element('#mqtt-status-state-topic').textContent, liveState.mqtt.state_topic);
  assert.equal(element('#mqtt-status-availability-topic').textContent, liveState.mqtt.availability_topic);
  liveState = {...liveState, mqtt: {...liveState.mqtt, connected: false}};
  await run('refresh()');
  assert.equal(element('#mqtt-status-message').textContent, 'Not configured yet');
  assert.equal(element('#mqtt-status-message').hidden, false);
  assert.equal(element('#mqtt-status-details').hidden, true, 'Disconnecting must hide MQTT details');
  assert.equal(element('#mqtt-status-broker').textContent, '—', 'Disconnecting must clear stale details');
  liveState = {...liveState, mqtt: {...liveState.mqtt, connected: true}};
  await run('refresh()');
  assert.equal(element('#mqtt-status-details').hidden, false, 'Reconnecting must restore MQTT details');
  assert.equal(element('#desk-setup-notice').hidden, true, 'Ready desks need no setup warning');
  assert.equal(element('#profile').textContent, 'Jiecang RJ12 / Maidesite');
  assert.equal(element('#height').textContent, '70 cm');
  assert.equal(element('#locked').textContent, 'Unavailable');
  assert.equal(element('#child-lock-control').disabled, true);
  for (const [locked, label] of [[true, 'Locked'], [false, 'Unlocked'], [null, 'Unavailable']]) {
    liveState = {...liveState, locked};
    await run('refresh()');
    assert.equal(element('#locked').textContent, label);
    for (const button of movementButtons) {
      assert.equal(button.disabled, locked === true && button.dataset.direction !== 'stop');
    }
    assert.equal(element('#target-go').disabled, locked === true);
    assert.equal(element('#target-height-input').disabled, locked === true);
    assert.equal(element('#target-height-slider').disabled, locked === true);
    assert.equal(element('#child-lock-control').disabled, true, 'Unknown/unsupported lock control stays disabled');
  }
  liveState = {...liveState, locked: false};
  liveCapabilities = {...liveCapabilities, child_lock: true};
  await run('refresh()');
  assert.equal(element('#child-lock-control').disabled, false);
  assert.equal(element('#child-lock-label').textContent, 'Lock desk');
  assert.equal(element('#child-lock-shackle').d, 'M7 11V7a5 5 0 0 1 10 0v4');
  const statusFetch = context.fetch;
  let lockRequests = 0;
  let finishLockRequest;
  context.fetch = (url, options) => {
    assert.equal(url, '/api/v1/commands/child-lock');
    ++lockRequests;
    const body = JSON.parse(options.body);
    assert.equal(body.locked, true);
    assert(body.request_id);
    return new Promise(resolve => { finishLockRequest = resolve; });
  };
  const locking = run('sendChildLock()');
  assert.equal(element('#child-lock-control').disabled, true);
  assert.equal(element('#child-lock-label').textContent, 'Locking…');
  assert.equal(element('#child-lock-shackle').d, 'M7 11V7a5 5 0 0 1 10 0v4');
  assert(movementButtons.filter(button => button.dataset.direction !== 'stop').every(button => button.disabled));
  assert.equal(movementButtons.find(button => button.dataset.direction === 'stop').disabled, false);
  await run('sendChildLock()');
  assert.equal(lockRequests, 1, 'Repeated clicks must not queue another lock change');
  finishLockRequest({ok: true, json: async () => ({locked: true})});
  await locking;
  assert.equal(element('#locked').textContent, 'Locked');
  assert.equal(element('#settings-toast').hidden, false);
  assert.equal(element('#settings-toast').className, 'alert alert-success');
  assert.equal(element('#toast-message').textContent, 'Desk locked successfully.');
  assert.equal(element('#child-lock-label').textContent, 'Unlock desk');
  assert.equal(element('#child-lock-shackle').d, 'M7 11V7a5 5 0 0 1 9.9-1');
  assert(movementButtons.filter(button => button.dataset.direction !== 'stop').every(button => button.disabled));
  assert.equal(movementButtons.find(button => button.dataset.direction === 'stop').disabled, false);
  assert.equal(element('#target-go').disabled, true, 'A confirmed lock disables movement immediately');
  assert.equal(element('#child-lock-control').disabled, false);
  for (const ok of [false, true]) {
    context.fetch = async () => ({ok, json: async () => ({accepted: true})});
    await run('sendChildLock()');
    assert.equal(element('#locked').textContent, 'Locked', 'Unconfirmed replies must not change the displayed state');
    assert.equal(element('#settings-toast').hidden, false);
    assert.equal(element('#settings-toast').className, 'alert alert-error');
    assert.match(element('#toast-message').textContent, /did not confirm/);
    assert.equal(element('#child-lock-control').disabled, false);
  }
  context.fetch = async (url, options) => {
    assert.equal(JSON.parse(options.body).locked, false);
    return {ok: true, json: async () => ({locked: false})};
  };
  await run('sendChildLock()');
  assert.equal(element('#locked').textContent, 'Unlocked');
  assert.equal(element('#settings-toast').className, 'alert alert-success');
  assert.equal(element('#toast-message').textContent, 'Desk unlocked successfully.');
  assert.equal(element('#child-lock-label').textContent, 'Lock desk');
  assert.equal(element('#child-lock-shackle').d, 'M7 11V7a5 5 0 0 1 10 0v4');
  assert(movementButtons.every(button => !button.disabled));
  assert.equal(element('#target-go').disabled, false, 'A confirmed unlock restores movement');
  context.fetch = statusFetch;
  liveCapabilities = {...liveCapabilities, presets: true};
  liveState = {...liveState, locked: false, presets: [72.5, 110, null, 129]};
  await run('refresh()');
  assert.equal(element('#preset-1').disabled, false);
  assert.equal(element('#preset-height-1').textContent, '72.5 cm');
  assert.equal(element('#preset-3').disabled, true, 'Unset memory cannot be recalled');
  assert.equal(element('#preset-4').disabled, true, 'Out-of-range memory cannot be recalled');
  assert.equal(element('#preset-save-submit').disabled, false, 'Empty memory can be saved');
  assert.equal(element('#preset-save-slot').disabled, false);
  const presetRequests = [];
  context.fetch = async (url, options) => {
    assert.equal(url, '/api/v1/commands/preset');
    presetRequests.push(JSON.parse(options.body));
    assert(presetRequests.at(-1).request_id);
    return {ok: true, json: async () => ({accepted: true})};
  };
  await run("sendPreset(1, 'recall')");
  await run("sendPreset(3, 'recall')");
  element('#preset-save-slot').value = '3';
  element('#preset-save-details').open = true;
  await run('saveCurrentHeight({preventDefault() {}})');
  assert.deepEqual(presetRequests.map(({preset, operation}) => [preset, operation]), [[1, 'recall'], [3, 'save']]);
  assert.equal(element('#preset-3').disabled, true, 'Save acceptance must not invent a confirmed height');
  assert.equal(element('#preset-save-details').open, false, 'Accepted save collapses the editor');
  assert.match(element('#toast-message').textContent, /Waiting for the controller/);
  for (const value of ['', '0', '5', '1.5', 'invalid']) {
    element('#preset-save-slot').value = value;
    await run('saveCurrentHeight({preventDefault() {}})');
  }
  assert.equal(presetRequests.length, 2, 'Invalid selections must never overwrite a preset');
  context.fetch = async () => ({ok: false, status: 409, json: async () => ({error: 'child_locked'})});
  element('#preset-save-slot').value = '2';
  element('#preset-save-details').open = true;
  await run('saveCurrentHeight({preventDefault() {}})');
  assert.match(element('#toast-message').textContent, /Child lock/);
  assert.equal(element('#preset-save-details').open, true, 'Failed saves leave the editor open for retry');
  assert.equal(element('#preset-height-2').textContent, '110.0 cm', 'Failed saves preserve the reported height');
  context.fetch = statusFetch;
  liveState = {...liveState, locked: true};
  await run('refresh()');
  assert.equal(element('#preset-1').disabled, true);
  assert.equal(element('#preset-save-submit').disabled, true);
  assert.equal(element('#preset-save-slot').disabled, true);
  assert.equal(movementButtons.find(button => button.dataset.direction === 'stop').disabled, false);
  liveState = {...liveState, locked: false};
  liveCapabilities = {...liveCapabilities, presets: false};
  await run('refresh()');
  assert.equal(element('#preset-save-submit').disabled, true, 'Unsupported profile disables both operations');
  const deviceSelectors = ['#network-mode', '#network-ssid', '#network-rssi', '#network-hostname',
    '#network-ip', '#network-mac', '#status-firmware-version', '#status-filesystem-version', '#device-uptime'];
  const snapshot = () => deviceSelectors.map(selector => element(selector).textContent);
  const initialSnapshot = snapshot();
  liveState = {...liveState, uptime: 9999, firmware_version: 'new', filesystem_version: 'new',
    network: {mode: 'ap', connected: false}, height: {value: 80, unit: 'cm'}, moving: true};
  await run('refresh()');
  assert.deepEqual(snapshot(), initialSnapshot, 'Polling must preserve the device snapshot');
  assert.equal(element('#mdns-notice').hidden, false);
  assert.equal(element('#height').textContent, '80 cm');
  assert.equal(element('#movement').textContent, 'Moving');
  assert(movementButtons.every(button => !button.disabled));
  assert.equal(element('#target-go').disabled, false);
  responseOk = false;
  await run('refresh()');
  assert.deepEqual(snapshot(), initialSnapshot, 'Connection loss must preserve the device snapshot');
  assert.equal(element('#mdns-notice').hidden, false);
  assert(movementButtons.every(button => button.disabled));
  assert.equal(element('#target-go').disabled, true);
  assert.equal(element('#child-lock-control').disabled, true, 'Connection loss disables lock changes');
  assert.equal(element('#desk-setup-notice').hidden, false);
  assert.equal(element('#state-label').textContent, 'No status connection');
  assert.equal(element('#desk-connection').textContent, 'Disconnected');
  assert.equal(element('#desk-connection')['data-connected'], 'false');
  assert.equal(element('#mqtt-status-message').textContent, 'Status unavailable');
  assert.equal(element('#mqtt-status-message').hidden, false);
  assert.equal(element('#mqtt-status-details').hidden, true, 'API failure must hide stale MQTT details');
  responseOk = true;
  liveState = {...liveState, moving: false};
  await run('refresh()');
  assert.equal(element('#movement').textContent, 'Stopped');
  assert.equal(element('#desk-connection').textContent, 'Connected');
  assert.equal(element('#desk-connection')['data-connected'], 'true');
  assert.equal(element('#mqtt-status-message').hidden, true);
  assert.equal(element('#mqtt-status-details').hidden, false, 'API recovery must restore MQTT details');
  assert(movementButtons.every(button => !button.disabled));
  assert.deepEqual(snapshot(), initialSnapshot, 'Reconnection must preserve the device snapshot');
  await run('refresh(true)'); // A page reload requests a new device snapshot.
  assert.equal(element('#network-mode').textContent, 'Setup access point');
  assert.equal(element('#device-uptime').textContent, '2h 46m 39s');
  assert.equal(element('#status-firmware-version').textContent, 'new');
  assert.equal(element('#mdns-notice').hidden, true);

  liveState = {...liveState, ready: false, profile: null};
  liveCapabilities = {profile_configured: false, move: false, stop: false, target_height: false};
  await run('refresh()');
  assert.equal(element('#profile').textContent, 'Not configured');
  assert.equal(element('#desk-connection').textContent, 'Disconnected');
  assert.equal(element('#desk-connection')['data-connected'], 'false');
  liveState = {...liveState, height: {value: null, unit: 'cm', quality: 'unknown'}};
  await run('refresh()');
  assert.equal(element('#height').textContent, 'Unknown');
  assert.equal(element('#state-label').textContent, 'Checking desk setup', 'Readiness does not indicate saved setup');
  for (const [profile, model] of [
    ['jiecang_rj12', 'JIECANG_GENERIC'], ['jiecang_rj12', 'JCHT35K72C'],
    ['jiecang_jarvis_rj45', 'FullyCB2C-A'],
    ['loctek_flexispot_rj45', 'FLEXISPOT_E7_PRO_PLUS']
  ]) {
    deskSettings = {profile, model};
    await run('loadDeviceSettings()');
    assert.equal(element('#desk-model').value, model);
    assert(element('#desk-model').options.some(option => option.value === model && !option.disabled));
    if (profile === 'jiecang_rj12') {
      assert.deepEqual(Array.from(element('#desk-model').options, option => option.value),
        ['', 'JIECANG_GENERIC', 'JCHT35K72C']);
      assert.match(element('#desk-model').options[2].text, /no target-height control/);
    }
    await run('refresh()');
    assert.equal(element('#desk-setup-notice').hidden, false);
    assert.equal(element('#state-label').textContent, 'Waiting for desk connection');
    assert.equal(element('#profile').textContent, element('#desk-profile').options.find(option => option.value === profile).text);
    assert.match(element('#state-message').textContent, /profile and model are saved/);
    assert(movementButtons.every(button => button.disabled), 'Saved setup must not unlock an unready desk');
    assert.equal(element('#target-go').disabled, true);
  }
  liveState = {...liveState, profile: 'future_profile'};
  await run('refresh()');
  assert.equal(element('#profile').textContent, 'future_profile', 'Unknown profiles retain their API identifier');
  liveState = {...liveState, profile: null};
  for (const settings of [{}, {profile: 'jiecang_rj12'}]) {
    deskSettings = settings;
    await run('loadDeviceSettings()');
    assert.equal(element('#state-label').textContent, 'Desk setup required');
  }
  deskSettings = {profile: 'loctek_flexispot_rj45', model: 'FLEXISPOT_E7_PRO_PLUS'};
  await run('loadDeviceSettings()');
  liveState = {...liveState, ready: true};
  liveCapabilities = {profile_configured: true, move: true, stop: true, target_height: false};
  await run('refresh()');
  assert.equal(element('#desk-setup-notice').hidden, true, 'The connection warning clears once the desk responds');
  assert(movementButtons.every(button => !button.disabled));
  assert.equal(element('#target-go').disabled, true, 'Readiness must preserve capability restrictions');
  context.fetch = settingsFetch;

  element('#led-brightness').value = '50';
  element('#wifi-ssid').value = 'Home';
  element('#wifi-password').value = 'test-password';
  element('#min-height').value = '60.0';
  element('#max-height').value = '125.0';
  element('#target-height-input').value = '70.0';
  element('#desk-profile').value = '';
  element('#desk-model').value = '';
  element('#mqtt-port').value = '1883';
  run('setTargetHeightBounds(60, 125)');
  for (const handler of ['sendMove("up")', 'sendTargetHeight()']) {
    responseOk = true;
    await run(handler);
    assert.equal(element('#settings-toast').hidden, false);
    assert.equal(element('#settings-toast').className, 'alert alert-success');
    assert.match(element('#toast-message').textContent, /accepted by the device/);
    responseOk = false;
    await run(handler);
    assert.equal(element('#settings-toast').className, 'alert alert-error');
    assert.match(element('#toast-message').textContent, /rejected/);
    assert.equal(timers.size, 0);
    context.fetch = async () => { throw new Error('Network unavailable'); };
    await run(handler);
    assert.equal(element('#toast-message').textContent, 'Network unavailable');
    assert.equal(element('#settings-toast').className, 'alert alert-error');
    context.fetch = settingsFetch;
  }
  responseOk = true;
  // Reload is covered separately by hostname checks; avoid a second mocked request.
  run('loadMqttSettings = async () => {}');
  for (const [handler, endpoint] of [
    ['saveDeviceSettings', 'device'], ['saveDeskSettings', 'device'],
    ['saveWifiSettings', 'wifi'], ['saveMqttSettings', 'mqtt']
  ]) {
    await run(`${handler}({preventDefault() {}})`);
    assert.equal(lastRequest.url, `/api/v1/settings/${endpoint}`);
    assert.equal(element('#settings-toast').hidden, false);
    assert.equal(element('#settings-toast').className, 'alert alert-success');
    assert.equal(timers.get(timerId).delay, 5000);
    assert(!html.includes(`id="${endpoint}-message"`), 'Save messages use the shared toast');
  }
  responseOk = false;
  await run('saveDeviceSettings({preventDefault() {}})');
  assert.equal(element('#settings-toast').className, 'alert alert-error');
  assert.equal(element('#device-save').disabled, false);
  assert.equal(timers.size, 0, 'Errors should remain visible');
  for (const kind of ['firmware', 'filesystem']) {
    const file = {name: `${kind}.bin`};
    element(`#${kind}-file`).files = [file];
    const updating = run(`uploadImage({preventDefault() {}}, '${kind}')`);
    assert.equal(lastUpload.url, `/api/v1/update/${kind}`);
    assert.equal(lastUpload.file, file);
    assert.equal(element('#update-dialog').open, true);
    assert.equal(element('#firmware-upload').disabled, true);
    assert.equal(element('#filesystem-upload').disabled, true);
    const count = uploadCount;
    await run(`uploadImage({preventDefault() {}}, '${kind}')`);
    assert.equal(uploadCount, count, 'A second update must not start during installation');
    lastUpload.upload.onprogress({lengthComputable: true, loaded: 25, total: 100});
    assert.equal(element('#upload-progress').value, 25);
    assert.equal(element('#install-progress').value, undefined, 'Installation has no invented percentage');
    lastUpload.upload.onload();
    assert.equal(element('#upload-progress').value, 100);
    assert.equal(element('#install-status').textContent, 'Verifying…');
    assert.equal(timers.size, 0, 'Uploading alone must not schedule a reload');
    lastUpload.status = 202;
    lastUpload.response = {installed: true, rebooting: true};
    lastUpload.onload();
    await updating;
    assert.equal(element('#install-progress').value, 100);
    assert.match(element('#update-message').textContent, /device will reboot/);
    const reloadsBeforeCountdown = reloadCount;
    for (let seconds = 5; seconds >= 0; seconds--) {
      assert.match(element('#update-message').textContent,
        new RegExp(`reload in ${seconds} ${seconds === 1 ? 'second' : 'seconds'}\\.$`));
      assert.equal(reloadCount, reloadsBeforeCountdown + (seconds === 0 ? 1 : 0));
      if (seconds > 0) {
        const {callback, delay} = timers.get(timerId);
        assert.equal(delay, 1000);
        timers.delete(timerId);
        callback();
      }
    }
    assert.equal(timers.size, 0, 'The countdown must stop at zero');
    run('updateBusy = false'); // A page reload creates a fresh script context.
  }
  assert.equal(reloadCount, 2);
  for (const failure of ['invalid', 'network', 'timeout', 'unconfirmed']) {
    const updating = run('uploadFirmware({preventDefault() {}})');
    if (failure === 'network') lastUpload.onerror();
    else if (failure === 'timeout') lastUpload.ontimeout();
    else {
      lastUpload.status = failure === 'invalid' ? 400 : 200;
      lastUpload.response = failure === 'invalid' ? {error:'not_pandadesk_firmware'} : {};
      lastUpload.onload();
    }
    await updating;
    assert.equal(element('#update-close').hidden, false);
    assert.equal(element('#update-message').className, 'error');
    assert.equal(element('#firmware-upload').disabled, false);
    assert.equal(element('#filesystem-upload').disabled, false);
    assert.equal(timers.size, 0, 'Failed or unconfirmed installation must not reload');
  }
  assert(html.indexOf('id="restart-device"') > html.indexOf('id="reset-message"'));
  assert.match(html, /<dialog id="restart-dialog"[^>]*>[\s\S]*?id="restart-message"[\s\S]*?<\/dialog>/);
  assert(html.includes("$('#restart-device').addEventListener('click', restartDevice)"));
  for (const failure of ['server', 'network', 'unconfirmed']) {
    context.fetch = async () => {
      if (failure === 'network') throw new Error('Network unavailable');
      return {ok: failure !== 'server', json: async () => ({})};
    };
    await run('restartDevice()');
    assert.equal(element('#restart-device').disabled, false, 'Failed restarts must allow retry');
    assert.equal(element('#restart-dialog').open, true);
    assert.equal(element('#restart-close').hidden, false, 'Failed restarts must allow closing the dialog');
    assert.equal(element('#restart-message').className, 'error');
    assert(element('#restart-message').textContent);
    assert.equal(timers.size, 0, 'Failed restarts must not reload');
    element('#restart-dialog').close();
  }
  let restartRequests = 0;
  context.fetch = async (url, options) => {
    ++restartRequests;
    assert.equal(url, '/api/v1/device/restart');
    assert.equal(options.method, 'POST');
    return {ok: true, json: async () => ({rebooting: true})};
  };
  const restarting = run('restartDevice()');
  assert.equal(element('#restart-device').disabled, true);
  assert.equal(element('#restart-dialog').open, true);
  assert.equal(element('#restart-close').hidden, true);
  assert.equal(element('#restart-title').textContent, 'Restarting device');
  assert.equal(element('#restart-message').className, '');
  await run('restartDevice()');
  await restarting;
  assert.equal(restartRequests, 1, 'Repeated clicks must not schedule multiple restarts');
  assert.match(element('#restart-message').textContent, /device is restarting/);
  for (let seconds = 5; seconds >= 0; seconds--) {
    assert.match(element('#restart-message').textContent,
      new RegExp(`reload in ${seconds} ${seconds === 1 ? 'second' : 'seconds'}\\.$`));
    assert.equal(reloadCount, seconds === 0 ? 3 : 2);
    if (seconds > 0) {
      const {callback, delay} = timers.get(timerId);
      assert.equal(delay, 1000);
      timers.delete(timerId);
      callback();
    }
  }
  assert.equal(timers.size, 0, 'The restart countdown must stop at zero');
  assert.equal(reloadCount, 3);
  console.log('Web UI checks passed: tap/hold movement, pointer/keyboard/cancellation/command ordering, Info cards/diagnostics/snapshots, compact Home, logs/navigation/polling/pause/restarts/downloads, live desk/MQTT status, controls, save alerts, update progress, guarded uploads, restart dialog, failure recovery and five-second countdowns.');
})().catch(error => { console.error(error); process.exitCode = 1; });
