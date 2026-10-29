// SPDX-License-Identifier: MIT
// Run the actual page script with a small DOM/fetch boundary; no RF simulator.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

async function main() {
  const html = fs.readFileSync(__dirname + '/index.html', 'utf8');
  const fields = Object.fromEntries([...html.matchAll(/id="([^"]+)"/g)].map(([, id]) => [id, {
    value: '', textContent: '', disabled: false, handlers: {},
    replaceChildren(...children) { this.children=children; },
    addEventListener(event, handler) { this.handlers[event] = handler; }
  }]));
  const buttons = [...html.matchAll(/<button[^>]+data-command="([^"]+)"/g)].map(([, command]) => ({
    dataset: {command}, addEventListener(event, handler) { this[event] = handler; }
  }));
  let data = {ready: true, valid_frames: 1,
    pairing: {saved: true, phase: 'complete', controller: 'F1010203', radiator: 'F1040506'},
    command: {phase: 'idle', attempts: 0, template_available: false}};
  data.radiators=[{id:1,name:'Living room',pairing:data.pairing,command:data.command}];
  data.capacity=8; data.exchange_id=0;
  let lastRequest, responseOK = true, deferredStatus;
  const context = vm.createContext({
    document: {
      getElementById(id) { assert(fields[id], id); return fields[id]; },
      createElement() { return {}; },
      querySelectorAll(selector) { return selector.includes('data-command') ? buttons : []; }
    },
    Date: class { getHours() { return 12; } getMinutes() { return 34; } getSeconds() { return 56; } },
    setTimeout() {}, clearTimeout() {},
    async fetch(url, options) {
      if (url.startsWith('/api/radio') && deferredStatus) { const next=deferredStatus;deferredStatus=undefined;return next; }
      if (url.startsWith('/api/radio')) return {ok: true, json: async () => data};
      lastRequest = {url, body: JSON.parse(options.body)};
      return {ok: responseOK, text: async () => 'No learned controller state', json: async () => ({frame: 'preview'})};
    }
  });
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  await new Promise(setImmediate);
  assert.match(fields.templateStatus.textContent, /No controller state learned/);
  assert.equal(fields.simulationPanel.hidden, true);
  fields.mode.value = '3'; fields.temperature.value = '15.5';
  await fields.sendTarget.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/command', body: {id: 1, mode: 3, centi_c: 1550, clock_seconds: 45296}});
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
  data.command.learned_available = true;
  data.command.saved_available = true;
  data.command.template_source = 'saved';
  delete data.command.template_age_ms;
  data.command.save_status = 'succeeded';
  await vm.runInContext('refreshRadio()', context);
  assert.match(fields.templateStatus.textContent, /Capture age unknown/);
  assert.match(fields.stateStorageStatus.textContent, /Latest explicit save: succeeded/);
  await fields.saveState.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/controller-state', body: {action: 'save', id: 1}});
  assert.match(fields.stateAction.textContent, /queued/);
  await fields.importState.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/controller-state', body: {action: 'import', id: 1, state: 'explicit-import'}});
  await fields.clearState.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/controller-state', body: {action: 'clear', id: 1}});
  data.command.save_status = 'failed'; data.command.state_store_error = 3;
  await vm.runInContext('refreshRadio()', context);
  assert.match(fields.stateStorageStatus.textContent, /Latest explicit save: failed/);
  assert.match(fields.stateStorageStatus.textContent, /previous usable state retained/);
  data.command.state_load_error = 2;
  await vm.runInContext('refreshRadio()', context);
  assert.match(fields.stateStorageStatus.textContent, /could not be loaded/);
  data.command.phase = 'waiting_report';
  await vm.runInContext('refreshRadio()', context);
  assert.equal(fields.sendTarget.disabled, true);
  data.command.phase = 'idle'; data.pairing.saved = false;
  await vm.runInContext('refreshRadio()', context);
  assert.equal(fields.sendTarget.disabled, true);
  responseOK = false;
  await fields.sendTarget.handlers.click();
  assert.equal(fields.commandAction.textContent, 'No learned controller state');
  data.simulation = true; data.scenario = 'delayed';
  await vm.runInContext('refreshRadio()', context);
  assert.equal(fields.simulationPanel.hidden, false);
  assert.match(fields.trafficNotice.textContent, /SIMULATION ONLY/);
  assert.equal(fields.source.value, 'F1010203');
  assert.equal(fields.destination.value, 'F1100001');
  responseOK = true;
  await fields.simulationFixture.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/simulation', body: {action: 'fixture'}});
  assert.match(fields.simulationAction.textContent, /not a radiator report/);
  await fields.simulationApply.handlers.click();
  assert.deepEqual(lastRequest, {url: '/api/simulation', body: {action: 'scenario', name: 'delayed'}});
  data.radiators.push({id:2,name:'Bedroom',pairing:{saved:true,phase:'complete',radiator:'F1040507'},command:{phase:'idle',template_source:'missing'}});
  fields.state.value='device-A-template';
  fields.radiatorSelect.value='2'; fields.radiatorSelect.handlers.change();
  await new Promise(setImmediate);
  assert.equal(fields.state.value,'');
  assert.match(fields.pairedReport.textContent,/No report/);
  assert.match(fields.templateStatus.textContent,/No controller state/);
  await fields.sendTarget.handlers.click(); assert.equal(lastRequest.body.id,2); assert.equal(lastRequest.body.state,undefined);
  await fields.clearState.handlers.click(); assert.equal(lastRequest.body.id,2);
  await fields.removeRadiator.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/radiators',body:{action:'remove',id:2}});
  // An old in-flight 404 from B must not reset a newer selection of A.
  let releaseStatus;
  deferredStatus=new Promise(resolve=>{releaseStatus=resolve;});
  const staleRefresh=vm.runInContext('refreshRadio()',context);
  fields.radiatorSelect.value='1'; fields.radiatorSelect.handlers.change();
  await new Promise(setImmediate);
  releaseStatus({status:404,ok:false}); await staleRefresh;
  assert.equal(vm.runInContext('selectedId',context),1);
  data.exchange_id=1; await vm.runInContext('refreshRadio()',context); assert.equal(fields.sendTarget.disabled,true);
  data.exchange_id=0; data.active_commands=1; data.pairing.saved=true;
  data.radiators[1].command.phase='waiting_report';
  await vm.runInContext('refreshRadio()',context);
  assert.equal(fields.sendTarget.disabled,false); // A selected, B pending.
  assert.equal(fields.pairStart.disabled,true);
  fields.radiatorSelect.value='2'; fields.radiatorSelect.handlers.change();
  await new Promise(setImmediate);
  assert.equal(fields.sendTarget.disabled,true);
  console.log('UI learned/missing state, optional import, preview, admission and error rendering: passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
