// SPDX-License-Identifier: MIT
// Included by test_admission: production owner, production HAL, fake IDF/CC1101.
static void test_recovery_bounds() {
    reset(); radio_worker w;
    status.ready=false; status.health=RADIO_INITIALIZING;
    status.control_enabled=false; begin_result=-42;
    w.step();
    assert(status.health==RADIO_INITIALIZING && status.recovery_attempt==1 && begin_count==1);
    const int delays[]={1000,2000,4000};
    for (unsigned i=0;i<3;++i) {
        assert(status.retry_at_ms==test_now_us/1000+delays[i]);
        test_now_us=status.retry_at_ms*1000-1;
        for (unsigned n=0;n<100;++n) w.step();
        assert(begin_count==i+1);
        ++test_now_us; w.step(); assert(begin_count==i+2);
    }
    assert(status.health==RADIO_FAULTED && !status.ready && status.recovery_attempt==4 && !status.retry_at_ms);
    test_now_us+=600000000; w.step(); assert(begin_count==4 && !transmit_count);
    assert(radio_retry()==ESP_OK && radio_retry()==ESP_ERR_INVALID_STATE);
    w.step(); assert(begin_count==5 && status.health==RADIO_RECOVERING);
    assert(radio_retry()==ESP_ERR_INVALID_STATE);
    begin_result=0; test_now_us=status.retry_at_ms*1000; w.step();
    assert(status.ready && status.health==RADIO_READY && status.recovery_successes==1);
    assert(status.failure_code==-42 && status.failure_stage==RADIO_FAILURE_INITIALIZE);
    assert(!status.control_enabled && !transmit_count && radio_retry()==ESP_ERR_INVALID_STATE);
    // Independent runtime failure gets a fresh finite batch, never old RF work.
    w.fail(-43); assert(status.health==RADIO_RECOVERING && status.recovery_attempt==0);
    auto due=status.retry_at_ms; w.step(); assert(begin_count==6);
    test_now_us=due*1000; w.step();
    assert(status.ready && status.recovery_attempt==1 && status.recovery_successes==2 && begin_count==7);
    status.recovery_attempts=status.recovery_successes=UINT32_MAX;
    w.fail(-44); test_now_us=status.retry_at_ms*1000; w.step();
    assert(status.recovery_attempts==UINT32_MAX && status.recovery_successes==UINT32_MAX && !transmit_count);
}
static uint8_t spi_fault_count(uint8_t count) { hal.record_error(-61); return count; }
static void test_radio_fault_observation_and_writes() {
    reset(); radio_worker w; status.control_enabled=false;
    hear(w,report_b(false));
    const auto received=status.report_time_us;
    const auto observation=registry.records[1].paired_report_time_us;
    const auto valid=status.valid_frames+1;
    rf_frame passive=report_b(false); passive.bytes[4]=8; reseal_test(passive); hear(w,passive);
    assert(radio_monitor_change("select",passive.bytes+1,"Retained")==ESP_OK);
    w.step(); assert(monitors.count==1);
    const auto selected=monitored[0].observation.time_us;
    count_hook=spi_fault_count; w.step();
    assert(!status.ready && status.failure_code==-61 && status.failure_stage==RADIO_FAILURE_RECEIVE);
    assert(status.valid_frames==valid && status.report_time_us==received);
    assert(!transmit_count && !status.control_enabled);
    // Local writes work while unavailable; failure outcomes are real store outcomes.
    assert(radio_device_rename(1,"Still local")==ESP_OK);
    w.step(); assert(!status.mutation_pending && !status.mutation_error && !strcmp(registry.records[0].name,"Still local"));
    assert(radio_mode_set(false)==ESP_OK); w.step(); assert(status.mode_save_result==RADIO_STORE_OK);
    assert(radio_device_rename(1,"Not stored")==ESP_OK); store_result=ESP_ERR_TIMEOUT;
    w.step(); assert(status.mutation_error==ESP_ERR_TIMEOUT && !strcmp(registry.records[0].name,"Still local"));
    count_hook=nullptr; test_now_us=status.retry_at_ms*1000; w.step();
    assert(status.ready && !status.control_enabled && !transmit_count);
    assert(registry.records[1].paired_report_time_us==observation && status.report_time_us==received);
    assert(monitors.count==1 && !strcmp(monitors.records[0].name,"Retained") && monitored[0].observation.time_us==selected);
    assert(status.valid_frames==valid);
}
static void test_fault_persistence_and_pairing() {
    reset(); radio_worker w; command.state=state_frame();
    assert(radio_control_start(1,&command)==ESP_OK); w.step();
    assert(radio_state_save(2,&b_state)==ESP_OK);
    // Fault before the queued local write: preserve pending status and execute it once.
    w.fail(-77); assert(mutation_pending && registry.records[1].state_save_result==RADIO_STORE_PENDING);
    w.step(); assert(!mutation_pending && registry.records[1].state_save_result==RADIO_STORE_OK);
    assert(registry.records[1].has_saved_template && !registry.records[0].command_reserved);
    test_now_us=status.retry_at_ms*1000; w.step();
    unsigned tx=transmit_count; w.step(); assert(transmit_count==tx);
    assert(radio_device_rename(1,"Committed")==ESP_OK);
    start_control(w,0); receive_result=-71;
    w.step(); // Commit rename, then TX/RX failure. Success must not be relabelled failure.
    assert(!strcmp(registry.records[0].name,"Committed") && !status.mutation_pending && !status.mutation_error);
    reset(); radio_worker queued_pair; uint32_t id;
    const uint8_t c[]={0xf1,1,2,3},candidate[]={0xf1,0x10,0,1};
    assert(radio_pair_start(c,candidate,&id)==ESP_OK);
    queued_pair.fail(-80); assert(!queue_count && !pair_reserved && !exchange_id && status.pairing.phase==RF_PAIR_ERROR);
    assert(status.pairing_error==-80 && !transmit_count);
    test_now_us=status.retry_at_ms*1000; queued_pair.step(); queued_pair.step(); assert(!transmit_count);
    assert(radio_pair_start(c,candidate,&id)==ESP_OK); queued_pair.step();
    assert(transmit_count==1); queued_pair.fail(-81);
    assert(status.pairing_error==RADIO_ERR_DELIVERY_UNCERTAIN && !pair_reserved);
    // Production ACK -> store succeeds -> restoring RX fails before publish().
    reset(); radio_worker saved_pair; saved_pair.pair.phase=RF_PAIR_WAIT_ACK;
    memcpy(saved_pair.pair.controller,c,4); memcpy(saved_pair.pair.radiator,registry.records[0].pair.radiator,4);
    saved_pair.pair.radiator[3]=8; saved_pair.pair.deadline_ms=test_now_us/1000+5000;
    pair_reserved=true; exchange_pending=false; exchange_id=status.pairing_id=3;
    rf_frame ack={}; assert(rf_parse_hex("0FF1040506C6F101020301000A47E6E64D5A",ack.bytes,sizeof(ack.bytes),&ack.size));
    ack.bytes[4]=8; reseal_test(ack); receive_result=-82; hear(saved_pair,ack);
    assert(!status.ready && saved_pair.saved && status.pairing_saved && status.pairing.phase==RF_PAIR_COMPLETE);
    assert(registry.count==3 && registry.records[2].id==3 && !pair_reserved && !exchange_id);
    reset(); radio_worker old_saved; old_saved.saved=true; old_saved.pair.phase=RF_PAIR_COMPLETE;
    status.pairing_saved=true;
    assert(radio_pair_start(c,candidate,&id)==ESP_OK); old_saved.fail(-84);
    assert(!status.pairing_saved && status.pairing.phase==RF_PAIR_ERROR && registry.count==2);
    // A confirmed readback remains confirmed even when its RX restart fails.
    reset(); radio_worker readback; start_control(readback,1); readback.step();
    hear(readback,report_b(false));
    receive_result=-85; hear(readback,report_b(true));
    assert(!status.ready && registry.records[1].control.phase==RF_CONTROL_MATCHED && !registry.records[1].command_reserved);
    // A pre-TX failure has not entered transmit(), so it is a confirmed
    // failure to attempt this frame, rather than uncertain physical delivery.
    reset(); radio_worker pre_tx; command.state=state_frame();
    assert(radio_control_start(1,&command)==ESP_OK); standby_result=-83; pre_tx.step();
    assert(!transmit_count && registry.records[0].control_error==-83 && !status.ready);
}
static void test_hal_containment() {
    IdfHal adapter; adapter.begin_operation(1200); adapter.spiBegin();
    uint8_t out[3]={1,2,3},in[3]={9,9,9};
    spi_starts=0; spi_start_result=ESP_ERR_INVALID_STATE;
    adapter.spiTransfer(out,3,in);
    assert(adapter.error()==ESP_ERR_INVALID_STATE && spi_starts==1 && !in[0] && !in[1]);
    adapter.spiTransfer(out,3,in); assert(spi_starts==1); // sticky, no further transfer
    spi_start_result=0; spi_end_result=ESP_ERR_TIMEOUT;
    adapter.begin_operation(1200); adapter.spiTransfer(out,3,in); assert(spi_starts==2 && adapter.error()==ESP_ERR_TIMEOUT);
    adapter.begin_operation(1200); adapter.spiTransfer(out,3,in); assert(spi_starts==2); // still in flight
    spi_end_result=0; adapter.begin_operation(1200); adapter.spiTransfer(out,3,in);
    assert(!adapter.error() && spi_starts==3);
    adapter.begin_operation(1); test_now_us+=1000; adapter.spiTransfer(out,3,in);
    assert(adapter.error()==ESP_ERR_TIMEOUT && spi_starts==3);
}
