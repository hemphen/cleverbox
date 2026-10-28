// SPDX-License-Identifier: MIT
// Run the actual page script with a small DOM/fetch boundary; no RF simulator.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

async function main() {
  const html = fs.readFileSync(__dirname + '/index.html', 'utf8');
  const fields = Object.fromEntries([...html.matchAll(/id="([^"]+)"/g)].map(([, id]) => [id, {
    value: '', textContent: '', disabled: false, handlers: {},
    addEventListener(event, handler) { this.handlers[event] = handler; }
  }]));
  const buttons = [...html.matchAll(/<button[^>]+data-command="([^"]+)"/g)].map(([, command]) => ({
    dataset: {command}, addEventListener(event, handler) { this[event] = handler; }
  }));
  let data = {ready: true, valid_frames: 1,
    pairing: {saved: true, phase: 'complete', controller: 'F1010203', radiator: 'F1040506'},
    command: {phase: 'idle', attempts: 0, template_available: false}};
  let lastRequest, responseOK = true;
  const context = vm.createContext({
    document: {
      getElementById(id) { assert(fields[id], id); return fields[id]; },
      querySelectorAll(selector) { return selector.includes('data-command') ? buttons : []; }
    },
    Date: class { getHours() { return 12; } getMinutes() { return 34; } getSeconds() { return 56; } },
    setTimeout() {},
    async fetch(url, options) {
      if (url === '/api/radio') return {ok: true, json: async () => data};
      lastRequest = {url, body: JSON.parse(options.body)};
      return {ok: responseOK, text: async () => 'No learned controller state', json: async () => ({frame: 'preview'})};
    }
  });
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  await new Promise(setImmediate);
  assert.match(fields.templateStatus.textContent, /No controller state learned/);
  fields.mode.value = '3'; fields.temperature.value = '15.5';
  await fields.sendTarget.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/command', body: {mode: 3, centi_c: 1550, clock_seconds: 45296}});
  assert.match(fields.commandAction.textContent, /delivery is not yet established/);
  fields.state.value = '  explicit-import  ';
  await fields.sendTarget.handlers.click();
  assert.equal(lastRequest.body.state, 'explicit-import');
  await buttons.find(button => button.dataset.command === 'target').click();
  assert.equal(lastRequest.url, '/api/preview');
  assert.equal(lastRequest.body.state, 'explicit-import');
  data.command.template_available = true; data.command.template_age_ms = 2500;
  await vm.runInContext('refreshRadio()', context);
  assert.match(fields.templateStatus.textContent, /learned 2.5 s ago \(RAM only\)/);
  data.command.phase = 'waiting_report';
  await vm.runInContext('refreshRadio()', context);
  assert.equal(fields.sendTarget.disabled, true);
  data.command.phase = 'idle'; data.pairing.saved = false;
  await vm.runInContext('refreshRadio()', context);
  assert.equal(fields.sendTarget.disabled, true);
  responseOK = false;
  await fields.sendTarget.handlers.click();
  assert.equal(fields.commandAction.textContent, 'No learned controller state');
  console.log('UI learned/missing state, optional import, preview, admission and error rendering: passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
