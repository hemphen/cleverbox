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
static void test_passive_reports() {
    reset(); radio_worker w; status.control_enabled=false;
    auto report=report_b(false); report.bytes[9]^=1; reseal_test(report); hear(w,report);
    assert(status.observation_count==1 && !registry.records[1].has_paired_report && !transmit_count);
    for (size_t i=0;i<RADIO_PASSIVE_CAPACITY+1;++i) {
        report.bytes[4]=(uint8_t)(32+i); reseal_test(report); hear(w,report);
    }
    assert(status.observation_count==RADIO_PASSIVE_CAPACITY && status.observation_evictions==2);
    assert(!transmit_count);
    reset(); radio_worker strict;
    assert(radio_control_start(2,&command)==ESP_OK); test_step(strict);
    report=report_b(false); report.bytes[9]^=1; reseal_test(report); hear(strict,report);
    assert(strict.controls[1].status.phase==RF_CONTROL_WAIT_REPORT && transmit_count==1 && !registry.records[1].has_paired_report);
    puts("Passive receive: bounded report inventory, listen-only silence and strict paired isolation: passed");
}
