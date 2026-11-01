// SPDX-License-Identifier: MIT
// Included in the production-worker harness. No parallel admission implementation.
#include <atomic>
#include <thread>
static void reseal_test(rf_frame &f) {
    auto crc = [](const uint8_t *p, size_t n, bool reflected) {
        uint16_t c=0xffff;
        while(n--) {
            c ^= reflected ? *p++ : (uint16_t)*p++ << 8;
            for(int i=0;i<8;++i) c=reflected ? (c>>1)^((c&1)?0xa001:0) :
                (uint16_t)(c<<1)^((c&0x8000)?0x8005:0);
        } return c;
    };
    size_t l=f.bytes[0]; uint16_t c=crc(f.bytes+1,l-2,true);
    f.bytes[l-1]=c; f.bytes[l]=c>>8; c=crc(f.bytes,l+1,false);
    f.bytes[l+1]=c>>8; f.bytes[l+2]=c;
}
static void mode_during_tx() { assert(radio_mode_set(false)==RADIO_ERR_MODE_BUSY); }
static void test_passive_mode() {
    reset(); radio_worker worker;
    assert(radio_mode_set(false)==ESP_OK && status.mode_pending && queue_count==1);
    assert(radio_control_start(1,&command)==RADIO_ERR_LISTEN_ONLY);
    const uint8_t c[]={0xf1,1,2,3}, dst[]={0xf1,0x10,0,1}; uint32_t id;
    assert(radio_pair_start(c,dst,&id)==RADIO_ERR_LISTEN_ONLY);
    test_step(worker); assert(!status.control_enabled && status.mode_save_result==RADIO_STORE_OK && !transmit_count);
    worker.start_tx(state_frame(),false,1); assert(!transmit_count && !tx_in_progress);
    hear(worker,state_frame());
    assert(status.candidate_count==1); // receive and storage remain available without TX
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,candidates[0].version,false)==ESP_OK);
    test_step(worker); assert(registry.records[0].has_saved_template && !transmit_count);
    assert(radio_mode_set(true)==ESP_OK); test_step(worker); assert(status.control_enabled && !transmit_count);
    assert(radio_control_start(1,&command)==ESP_OK);
    assert(radio_mode_set(false)==RADIO_ERR_MODE_BUSY); test_step(worker); assert(transmit_count==1);
    assert(radio_mode_set(false)==RADIO_ERR_MODE_BUSY);
    test_now_us += RF_CONTROL_WAIT_MS*1000LL; test_step(worker);
    assert(!registry.records[0].command_reserved);
    assert(radio_mode_set(false)==ESP_OK); test_step(worker);
    assert(!status.control_enabled && transmit_count==1);
    for(int error : {ESP_ERR_INVALID_STATE,ESP_ERR_TIMEOUT}) {
        mode_store_result=error; assert(radio_mode_set(true)==ESP_OK); test_step(worker);
        assert(!status.control_enabled && status.mode_error==error && status.mode_persistence_uncertain);
        assert(status.mode_save_result==RADIO_STORE_FAILED && radio_control_start(1,&command)==RADIO_ERR_LISTEN_ONLY);
    }
    mode_store_result=0; assert(radio_mode_set(true)==ESP_OK); test_step(worker);
    assert(!status.mode_error && !status.mode_persistence_uncertain && status.control_enabled);
    transmit_hook=mode_during_tx;
    worker.start_tx(state_frame(),false,1); assert(tx_in_progress); worker.step(); assert(!tx_in_progress);
    transmit_hook=nullptr;
    assert(radio_pair_start(c,dst,&id)==ESP_OK);
    assert(radio_mode_set(false)==RADIO_ERR_MODE_BUSY);
    assert(radio_pair_cancel(id)==ESP_OK); test_step(worker); test_step(worker);
    assert(radio_mode_set(false)==ESP_OK); test_step(worker);
    // Real task mutex; contenders start together. Exactly one reserves admission.
    for(unsigned n=0;n<80;++n) {
        reset(); std::atomic<unsigned> ready{0}; std::atomic<bool> go{false};
        int mode_result=-1,command_result=-1;
        std::thread a([&]{++ready;while(!go.load()) std::this_thread::yield();mode_result=radio_mode_set(false);});
        std::thread b([&]{++ready;while(!go.load()) std::this_thread::yield();command_result=radio_control_start(1,&command);});
        while(ready.load()!=2) std::this_thread::yield();
        go=true; a.join(); b.join();
        assert((mode_result==ESP_OK && command_result==RADIO_ERR_LISTEN_ONLY) ||
               (mode_result==RADIO_ERR_MODE_BUSY && command_result==ESP_OK));
        assert(queue_count==1);
    }
    reset(); radio_worker guarded; status.control_enabled=false;
    start_control(guarded,0); test_step(guarded); // bypass admission deliberately
    assert(!transmit_count && registry.records[0].control.phase==RF_CONTROL_ERROR);
    assert(registry.records[0].control_error==RADIO_ERR_LISTEN_ONLY);
    reset(); radio_worker pair_guard; status.control_enabled=false;
    assert(rf_pair_start(&pair_guard.pair,c,dst,test_now_us/1000));
    test_step(pair_guard); assert(!transmit_count && pair_guard.pair.phase==RF_PAIR_ERROR);
    puts("Listen-only admission, final TX boundary, busy/failed transitions, no replay and concurrent ordering: passed");
}
static void test_passive_capture() {
    reset(); radio_worker worker; status.control_enabled=false;
    rf_frame state=state_frame(); state.bytes[4]^=1; reseal_test(state);
    hear(worker,state);
    assert(status.candidate_count==1 && !registry.records[0].has_control_template);
    const auto original=candidates[0];
    rf_frame report=report_b(false); report.bytes[9]^=1; reseal_test(report);
    hear(worker,report);
    assert(status.observation_count==1 && !registry.records[1].has_paired_report);
    assert(!memcmp(observations[0].report.source,report.bytes+1,4));
    rf_frame ack={}; assert(rf_parse_hex("0FF1040506C6F101020301000A47E6E64D5A",ack.bytes,sizeof(ack.bytes),&ack.size));
    hear(worker,ack); rf_frame bad=state; bad.bytes[57]^=1; hear(worker,bad);
    bad=state; bad.bytes[10]^=1; reseal_test(bad); hear(worker,bad);
    assert(status.candidate_count==1 && !memcmp(&candidates[0],&original,sizeof(original)));
    assert(!registry.records[0].has_saved_template && !mutation_pending && !transmit_count);
    assert(radio_candidate_adopt(2,registry.records[0].pair.radiator,original.version,false)==ESP_ERR_INVALID_ARG);
    hear(worker,state); // replacement invalidates even an identical capture selection
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,original.version,false)==RADIO_ERR_CANDIDATE_CHANGED);
    auto selected=candidates[0];
    registry.records[0].has_control_template=true;
    memcpy(registry.records[0].control_template,state_frame().bytes,CONTROLLER_STATE_BYTES);
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,selected.version,false)==ESP_OK);
    state.bytes[32]^=1; reseal_test(state); hear(worker,state); // save snapshot then receive replacement
    assert(!memcmp(registry.records[0].saved_template,selected.state,59) && !registry.records[0].has_control_template);
    assert(status.adoption_result==RADIO_STORE_OK && !strcmp(status.adoption_version,selected.version));
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,candidates[0].version,false)==RADIO_ERR_REPLACE_REQUIRED);
    store_result=ESP_ERR_TIMEOUT;
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,candidates[0].version,true)==ESP_OK);
    test_step(worker); assert(status.adoption_result==RADIO_STORE_FAILED && status.adoption_error==ESP_ERR_TIMEOUT);
    assert(!memcmp(registry.records[0].saved_template,selected.state,59)); store_result=0;
    // Independent bounds, least-recent capture/report, low-slot tie break.
    for(size_t i=0;i<RADIO_PASSIVE_CAPACITY+1;++i) {
        state.bytes[9]=(uint8_t)(32+i); reseal_test(state); hear(worker,state);
        report.bytes[4]=(uint8_t)(32+i); reseal_test(report); hear(worker,report);
    }
    assert(status.candidate_count==RADIO_PASSIVE_CAPACITY && status.observation_count==RADIO_PASSIVE_CAPACITY);
    assert(status.candidate_evictions==2 && status.observation_evictions==2);
    assert(radio_candidate_adopt(1,registry.records[0].pair.radiator,selected.version,true)==RADIO_ERR_CANDIDATE_CHANGED);
    assert(!transmit_count);
    // A report to another controller cannot advance an actual paired command.
    reset(); radio_worker strict;
    assert(radio_control_start(2,&command)==ESP_OK); test_step(strict);
    report=report_b(false); report.bytes[9]^=1; reseal_test(report); hear(strict,report);
    assert(strict.controls[1].status.phase==RF_CONTROL_WAIT_REPORT && transmit_count==1 && !registry.records[1].has_paired_report);
    puts("Passive production receive: separate cache bounds, strict paired isolation, candidate versions, exact snapshots and adoption errors: passed");
    printf("Passive RAM: observation=%zu candidate=%zu caches=%zu status=%zu request=%zu\n",sizeof(radio_observation),sizeof(radio_candidate),sizeof(observations)+sizeof(candidates),sizeof(status),sizeof(radio_request));
}
