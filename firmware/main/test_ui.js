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
  let network = {http_interface:'sta',http_control_allowed:true,state:'disconnected',configured:false,recovery_ap:true,attempts:0,setup_result:'none',error:'none',ip:''};
  let networkReadsFail=false, confirmResult=true;
  let clock={usable:false,source:'unsynchronized',timezone:'UTC0',synchronized_this_boot:false,sync_age_ms:null};
  let commissioning={mode:'control-enabled',pending:false,save_status:'succeeded',storage_error:0,capacity:16,
    observations:[],candidates:[],observation_evictions:0,candidate_evictions:0,adoption:{status:'not_attempted'}};
  let mqtt={configured:true,enabled:true,connected:false,bridge_id:'020000000001'};
  let lastRequest, responseOK = true, deferredStatus;
  const context = vm.createContext({
    document: {
      getElementById(id) { assert(fields[id], id); return fields[id]; },
      createElement() { return {}; },
      querySelectorAll(selector) { return selector.includes('data-command') ? buttons : []; }
    },
    Date,
    TextEncoder,
    confirm(){return confirmResult;},
    setTimeout() {}, clearTimeout() {},
    async fetch(url, options) {
      if(url==='/api/commissioning' && !options.body) return {ok:true,json:async()=>commissioning};
      if(url==='/api/mqtt' && !options.body) return {ok:true,json:async()=>mqtt};
      if(url==='/api/time' && !options.body) return {ok:true,json:async()=>clock};
      if(url==='/api/network' && !options.body){
        if(networkReadsFail) throw new Error('unreachable');
        return {ok:true,json:async()=>network};
      }
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
  assert.deepEqual(lastRequest, {url: '/api/command', body: {id: 1, mode: 3, centi_c: 1550}});
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
  assert.match(fields.networkStatus.textContent,/Wi-Fi: disconnected/);
  assert.match(fields.networkAccess.textContent,/Protected Cleverbox AP available/);
  assert.match(html, /id="wifiPassword" type="password"/);
  fields.wifiSsid.value=''; fields.wifiPassword.value='x';
  const before=lastRequest;
  await fields.wifiSave.handlers.click(); assert.equal(lastRequest,before);
  assert.equal(fields.wifiPassword.value,'');
  fields.wifiSsid.value='ü'.repeat(17); fields.wifiPassword.value=String.fromCharCode(...Array.from({length:12},(_,i)=>65+i));
  await fields.wifiSave.handlers.click(); assert.equal(lastRequest,before); // Bytes, not character count.
  fields.wifiSsid.value=String.fromCharCode(...Array.from({length:12},(_,i)=>65+i));
  const secret=String.fromCharCode(...Array.from({length:12},(_,i)=>97+i));
  fields.wifiPassword.value=secret;
  await fields.wifiSave.handlers.click();
  assert.equal(lastRequest.url,'/api/network'); assert.equal(lastRequest.body.action,'configure');
  assert.equal(lastRequest.body.password,secret); assert.equal(fields.wifiPassword.value,'');
  assert.match(fields.networkAction.textContent,/Connection queued/);
  network={...network,state:'connecting',setup_result:'pending',attempts:1};
  await vm.runInContext('refreshNetwork()',context); assert.equal(fields.wifiSave.disabled,true);
  network={...network,state:'failed',setup_result:'failed',error:'connection_failed'};
  await vm.runInContext('refreshNetwork()',context); assert.equal(fields.wifiSave.disabled,false);
  assert.match(fields.networkStatus.textContent,/Setup result: failed/);
  network={...network,state:'connected',configured:true,ip:'192.0.2.10',setup_result:'saved',error:'none'};
  await vm.runInContext('refreshNetwork()',context);
  assert.match(fields.networkAccess.textContent,/http:\/\/192.0.2.10/);
  assert.equal(fields.wifiPassword.value,'');
  network={...network,persistence_uncertain:true,error:'persistence_uncertain'};
  await vm.runInContext('refreshNetwork()',context);
  assert.match(fields.networkStatus.textContent,/unknown after restart/);
  assert.match(fields.networkAccess.textContent,/Setup access stays open/);
  network={...network,persistence_uncertain:false,error:'none'};
  networkReadsFail=true; await vm.runInContext('refreshNetwork()',context);
  assert.match(fields.networkStatus.textContent,/join the home network/);
  assert.match(fields.networkAccess.textContent,/192.0.2.10/); // Retain last station address across AP closure.
  networkReadsFail=false; responseOK=false; fields.wifiPassword.value=secret;
  await fields.wifiSave.handlers.click(); assert.equal(fields.wifiPassword.value,'');
  assert.match(fields.networkAction.textContent,/failed or is busy/);
  responseOK=true; confirmResult=false;
  const beforeCancel=lastRequest; await fields.wifiForget.handlers.click();
  assert.equal(lastRequest,beforeCancel);
  confirmResult=true; await fields.wifiForget.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/network',body:{action:'forget',confirm:true}});
  network={...network,manual_recovery:true}; await vm.runInContext('refreshNetwork()',context);
  assert.match(fields.networkStatus.textContent,/Manual recovery: active/);
  assert.match(fields.networkAccess.textContent,/Manual recovery keeps AP open/);
  await fields.wifiFinishRecovery.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/network',body:{action:'finish_recovery'}});
  assert.match(fields.networkAction.textContent,/Finish recovery queued/);
  confirmResult=false;
  const beforeFinish=lastRequest; await fields.wifiFinishRecovery.handlers.click();
  assert.equal(lastRequest,beforeFinish);
  confirmResult=true;
  network={...network,http_interface:'ap',http_control_allowed:false,ap_closing:true};
  await vm.runInContext('refreshNetwork()',context);
  await vm.runInContext('refreshRadio()',context); await vm.runInContext('refreshMqtt()',context);
  assert.match(fields.httpAccess.textContent,/diagnostics only/);
  assert.match(fields.networkAccess.textContent,/even offline/);
  for(const id of ['sendTarget','pairStart','saveState','removeRadiator','saveTimezone','mqttSave']) assert(fields[id].disabled,id);
  const beforeBlocked=lastRequest;
  for(const id of ['sendTarget','clearState','removeRadiator','saveTimezone','mqttSave']) await fields[id].handlers.click();
  assert.equal(lastRequest,beforeBlocked);
  assert.match(fields.commandAction.textContent,/Unavailable here/);
  network={...network,http_interface:'sta',http_control_allowed:true};
  await vm.runInContext('refreshNetwork()',context);
  network={...network,recovery_ap:false,recovery_error:'ap_open_failed'};
  await vm.runInContext('refreshNetwork()',context);
  assert.match(fields.networkStatus.textContent,/Recovery error: ap_open_failed/);
  assert.match(html,/WPA2 password <strong>cleverbox/);
  await fields.mqttDiscoveryEnable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'discovery_enable'}});
  await fields.mqttDiscoveryDisable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'discovery_disable'}});
  mqtt={...mqtt,discovery_enabled:false,discovery_pending:2,discovery_error:0x7413};
  await vm.runInContext('refreshMqtt()',context);
  assert.match(fields.mqttStatus.textContent,/cleanup pending: 2/);
  assert.match(fields.mqttStatus.textContent,/publication failed; retrying/);
  for(const field of Object.values(fields)) assert(!field.textContent.includes(secret));
  await vm.runInContext('refreshTime()',context);
  assert.match(fields.timeStatus.textContent,/Time unavailable/);
  assert.match(fields.timeStatus.textContent,/Synchronized this boot: no/);
  clock={...clock,usable:true,source:'manual',local:'2026-01-01T12:00:00+0000',utc:'2026-01-01T12:00:00Z'};
  await vm.runInContext('refreshTime()',context);
  assert.match(fields.timeStatus.textContent,/Source: manual/);
  clock={...clock,source:'network',synchronized_this_boot:true,sync_age_ms:61000};
  await vm.runInContext('refreshTime()',context);
  assert.match(fields.timeStatus.textContent,/latest sync 61 s ago/);
  assert.match(fields.timeStatus.textContent,/Offline/);
  fields.timezone.value='CET-1CEST,M3.5.0/2,M10.5.0/3';
  await fields.saveTimezone.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/time',body:{action:'timezone',timezone:fields.timezone.value}});
  fields.manualUtc.value=''; const beforeTime=lastRequest;
  await fields.setManualTime.handlers.click(); assert.equal(lastRequest,beforeTime);
  fields.manualUtc.value='2026-01-01T12:00:00';
  await fields.setManualTime.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/time',body:{action:'manual',unix_seconds:1767268800}});
  assert.match(fields.timeAction.textContent,/Manual UTC time set/);
  responseOK=false; await fields.saveTimezone.handlers.click();
  assert.equal(fields.timeAction.textContent,'No learned controller state');
  assert(!html.includes('getHours()')); assert(!html.includes('clock_seconds:'));
  responseOK=true;
  mqtt={...mqtt,broker_connected:true,subscription_pending:true};
  await vm.runInContext('refreshMqtt()',context); assert.match(fields.mqttStatus.textContent,/waiting for command subscription/);
  mqtt={...mqtt,broker_connected:false,subscription_pending:false,subscription_error:0x7401};
  await vm.runInContext('refreshMqtt()',context); assert.match(fields.mqttStatus.textContent,/rejected/);
  mqtt.subscription_error=0x7402;
  await vm.runInContext('refreshMqtt()',context); assert.match(fields.mqttStatus.textContent,/acknowledgement timed out/);
  mqtt={...mqtt,connected:true,subscription_error:0};
  await vm.runInContext('refreshMqtt()',context); assert.match(fields.mqttStatus.textContent,/MQTT: ready/);
  fields.mqttHost.value='broker.test'; fields.mqttPort.value='1883'; fields.mqttUser.value='user';
  fields.mqttPassword.value=secret; fields.mqttEnabled.value='true';
  await fields.mqttSave.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'configure',host:'broker.test',port:1883,username:'user',password:secret,enabled:true}});
  assert.equal(fields.mqttPassword.value,'');
  await fields.mqttDisable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'disable'}});
  await fields.mqttEnable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'enable'}});
  await fields.mqttForget.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'forget'}});
  await fields.mqttDiscoveryEnable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'discovery_enable'}});
  await fields.mqttDiscoveryDisable.handlers.click(); assert.deepEqual(lastRequest,{url:'/api/mqtt',body:{action:'discovery_disable'}});
  mqtt={...mqtt,discovery_enabled:false,discovery_pending:2,discovery_error:0x7413};
  await vm.runInContext('refreshMqtt()',context);
  assert.match(fields.mqttStatus.textContent,/cleanup pending: 2/);
  assert.match(fields.mqttStatus.textContent,/publication failed; retrying/);
  for(const field of Object.values(fields)) assert(!field.textContent.includes(secret));
  commissioning.mode='listen-only';
  commissioning.observations=[{radiator:'F1040506',controller:'F10A0B0C',age_ms:300001,stale:true,mode:0,temperature_c:null,target_c:20.5,paired:true,paired_id:1,saved_template:true}];
  commissioning.candidates=[{radiator:'F1040506',controller:'F10A0B0C',version:'A'.repeat(32),age_ms:4500,paired:true,paired_id:1}];
  await vm.runInContext('refreshCommissioning(true)',context);
  assert.equal(fields.sendTarget.disabled,true); assert.equal(fields.pairStart.disabled,true);
  assert.match(fields.passiveObservations.textContent,/F1040506 → F10A0B0C.*STALE/);
  assert.match(fields.passiveObservations.textContent,/Measured: unavailable/);
  assert.match(fields.passiveObservations.textContent,/Candidate: F10A0B0C/);
  assert.equal(fields.adoptCandidate.disabled,false);
  fields.replaceCandidate.checked=true;
  await fields.adoptCandidate.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/controller-state',body:{action:'adopt',id:1,radiator:'F1040506',version:'A'.repeat(32),replace:true}});
  commissioning.candidates[0]={...commissioning.candidates[0],version:'B'.repeat(32)};
  await vm.runInContext('refreshCommissioning()',context);
  assert.equal(fields.candidateSelect.value,'A'.repeat(32));
  assert.equal(fields.adoptCandidate.disabled,true);
  assert.match(fields.candidateDetails.textContent,/Replaced or evicted/);
  await fields.refreshCandidates.handlers.click();
  assert.equal(fields.candidateSelect.value,'B'.repeat(32)); assert.equal(fields.replaceCandidate.checked,false);
  commissioning.save_status='failed';commissioning.storage_error=6;commissioning.persistence_uncertain=true;
  commissioning.adoption={id:1,version:'A'.repeat(32),status:'failed',error:6};
  await vm.runInContext('refreshCommissioning()',context);
  assert.match(fields.commissioningStatus.textContent,/RF disabled/);
  assert.match(fields.commissioningStatus.textContent,/Persistence uncertain/);
  assert.match(fields.adoptionStatus.textContent,/failed.*ID 1.*Storage error/);
  network.http_control_allowed=false; network.http_interface='ap';
  await vm.runInContext('refreshNetwork()',context);
  await vm.runInContext('refreshCommissioning()',context);
  assert.equal(fields.enableControl.disabled,true); assert.equal(fields.adoptCandidate.disabled,true);
  network={...network,http_interface:'sta',http_control_allowed:true};
  await vm.runInContext('refreshNetwork()',context);
  commissioning={...commissioning,monitor_capacity:8,monitor_save_status:'succeeded',monitor_storage_error:0,
    monitors:[{radiator:'F1040506',name:'Observed room',available:false}],discovery_enabled:false};
  await vm.runInContext('refreshCommissioning()',context);
  assert.match(fields.monitorStatus.textContent,/Global discovery: disabled/);
  assert.match(fields.monitorStatus.textContent,/F1040506 · Observed room · No report this boot/);
  fields.monitorAddress.value='F1040506'; fields.monitorName.value='Renamed';
  await fields.monitorSelect.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/commissioning',body:{action:'select',radiator:'F1040506',name:'Renamed'}});
  await fields.monitorRename.handlers.click(); assert.equal(lastRequest.body.action,'rename');
  await fields.monitorRemove.handlers.click();
  assert.deepEqual(lastRequest,{url:'/api/commissioning',body:{action:'remove',radiator:'F1040506'}});
  commissioning.monitor_storage_error=1; commissioning.monitor_persistence_uncertain=true; commissioning.monitor_save_status='failed';
  await vm.runInContext('refreshCommissioning()',context);
  assert.match(fields.monitorStatus.textContent,/Persistence uncertain/);
  for(const id of ['monitorSelect','monitorRename','monitorRemove']) assert(fields[id].disabled,id);
  network={...network,http_interface:'ap',http_control_allowed:false}; await vm.runInContext('refreshNetwork()',context);
  const beforeMonitor=lastRequest; await fields.monitorSelect.handlers.click(); assert.equal(lastRequest,beforeMonitor);
  console.log('UI passive mode/candidate selection/errors plus learned/missing state, optional import, preview, admission and error rendering: passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
