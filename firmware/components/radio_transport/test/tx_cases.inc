// SPDX-License-Identifier: MIT
// Raw production iterations: no automatic completion between deterministic events.
static void tx_busy(uint8_t bytes = 16) { tx_gpio_high=true; tx_marc=0x13; tx_fifo_count=bytes; }
static void tx_done() { tx_gpio_high=false; tx_marc=1; tx_fifo_count=0; }
static void ready_state(radio_worker &w, unsigned i) {
    start_control(w,i);
    rf_control_sent(&w.controls[i],test_now_us/1000,true); // Poll already completed in this fixture.
    rf_frame f=report_b(false);
    if (!i) { --f.bytes[4]; reseal_test(f); }
    rf_control_receive(&w.controls[i],test_now_us/1000,&f);
    assert(w.controls[i].status.phase==RF_CONTROL_SEND);
}
static void test_tx_lifecycle() {
    reset(); radio_worker w; start_control(w,1); tx_busy();
    w.step();
    assert(tx_in_progress && w.tx.active && w.tx.id==2 && w.tx.size==16);
    assert(transmit_count==1 && !finish_count && !receiving && packet_mode==0 && gdo_mapping==6);
    assert(w.controls[1].status.phase==RF_CONTROL_POLL && registry.records[1].command_reserved);
    assert(!tx_status_reads); // start returns to the task without completion polling.
    const auto rx_reads=count_reads, starts=receive_starts;
    const auto deadline=w.tx.deadline_us;
    for (unsigned i=0;i<20;++i) {
        test_now_us+=1000; w.step();
        assert(w.tx.deadline_us==deadline && tx_in_progress && transmit_count==1);
        assert(count_reads==rx_reads && receive_starts==starts && !finish_count);
    }
    assert(tx_status_reads==40);
    tx_done(); w.step();
    assert(!w.tx.active && !tx_in_progress && finish_count==1 && receive_starts==starts+1);
    assert(receiving && packet_mode==2 && gdo_mapping==0x2e);
    assert(registry.records[1].control.phase==RF_CONTROL_WAIT_REPORT && !registry.records[1].control.attempts);
    w.step(); assert(transmit_count==1 && finish_count==1 && receive_starts==starts+1);
    // A state frame retains its original bytes until completion. The ready
    // deadline can pass after start; it cannot erase the one physical attempt.
    rf_frame report=report_b(false);
    rf_control_receive(&w.controls[1],test_now_us/1000,&report);
    w.controls[1].deadline_ms=test_now_us/1000+1;
    tx_busy(59); w.step();
    auto bytes=w.tx; assert(bytes.size==59 && !w.controls[1].status.attempts);
    test_now_us+=5000; w.step();
    assert(w.controls[1].status.phase==RF_CONTROL_SEND && registry.records[1].command_reserved);
    assert(!memcmp(w.tx.bytes,bytes.bytes,bytes.size));
    tx_done(); w.step();
    assert(registry.records[1].control.phase==RF_CONTROL_WAIT_READBACK && registry.records[1].control.attempts==1);
    assert(w.controls[1].deadline_ms==test_now_us/1000+RF_CONTROL_WAIT_MS);
    w.step(); assert(registry.records[1].control.attempts==1 && transmit_count==2 && finish_count==2);
}
static void test_tx_signal_and_deadline() {
    // Whole pulse already over at first poll: IDLE + stable empty FIFO proves
    // progress, without requiring the task to have seen a rising edge.
    reset(); radio_worker immediate; start_control(immediate,0); immediate.step();
    assert(immediate.tx.active && !finish_count); immediate.step();
    assert(!immediate.tx.active && finish_count==1 && status.ready);
    for (unsigned fault=0;fault<4;++fault) {
        reset(); radio_worker w; ready_state(w,1); tx_busy(59); w.step();
        if (fault==0) { tx_done(); tx_gpio_high=true; } // Stuck high, even when chip says done.
        if (fault==1) tx_gpio_high=false; // Missing pulse, chip/FIFO still busy.
        if (fault==2) { tx_done(); tx_fifo_count=1; } // IDLE with undrained FIFO is not success.
        // fault 3 keeps GDO asserted and MARCSTATE in TX.
        auto deadline=w.tx.deadline_us; test_now_us=deadline-1; w.step();
        assert(w.tx.active && !finish_count && w.tx.deadline_us==deadline);
        ++test_now_us; w.step();
        assert(!w.tx.active && !tx_in_progress && !status.ready && !finish_count);
        assert(status.failure_stage==RADIO_FAILURE_TX_COMPLETE && status.failure_code==RADIOLIB_ERR_TX_TIMEOUT);
        assert(registry.records[1].control.attempts==1 && registry.records[1].control_error==RADIO_ERR_DELIVERY_UNCERTAIN);
        assert(receiving && packet_mode==2 && gdo_mapping==0x2e && receive_starts==1);
    }
}
static void poll_spi_fault() { hal.record_error(-94); }
static void poll_expiry() { test_now_us+=100000; }
static void poll_unstable() { tx_fifo_count=tx_status_reads%2; }
static void test_tx_failures() {
    for (bool strobe : {false,true}) {
        reset(); radio_worker w; ready_state(w,1); tx_strobe=strobe; transmit_result=-91;
        w.step();
        assert(!w.tx.active && !tx_in_progress && !status.ready && transmit_count==1);
        assert(status.failure_stage==RADIO_FAILURE_TX_START && status.failure_code==-91);
        assert(registry.records[1].control.attempts==unsigned(strobe));
        assert(registry.records[1].control_error==(strobe ? RADIO_ERR_DELIVERY_UNCERTAIN : -91));
        assert(receive_starts==1 && receiving && packet_mode==2);
    }
    for (unsigned fault=0;fault<7;++fault) {
        reset(); radio_worker w; ready_state(w,1); w.step();
        int error=-93; radio_failure_stage stage=RADIO_FAILURE_TX_COMPLETE;
        if (fault==0) finish_result=error;
        if (fault==1) { poll_hook=poll_spi_fault; error=-94; }
        if (fault==2) { receive_result=error=-95; stage=RADIO_FAILURE_RX_RESTORE; }
        if (fault==3) { tx_fifo_count=0x80; error=RADIO_ERR_TX_UNDERFLOW; }
        if (fault==4) { tx_marc=0x16; error=RADIO_ERR_TX_UNDERFLOW; }
        if (fault==5) { poll_hook=poll_expiry; error=RADIOLIB_ERR_TX_TIMEOUT; }
        if (fault==6) { finish_result=error; receive_result=-96; } // Original code survives failed cleanup.
        w.step();
        assert(!status.ready && !w.tx.active && !tx_in_progress);
        assert(status.failure_stage==stage && status.failure_code==error);
        assert(registry.records[1].control.phase==RF_CONTROL_ERROR && registry.records[1].control.attempts==1);
        assert(registry.records[1].control_error==RADIO_ERR_DELIVERY_UNCERTAIN && !registry.records[1].command_reserved);
        assert(receive_starts==1); // Actual restoration attempted once, even after sticky poll error.
    }
    reset(); radio_worker unstable; start_control(unstable,0); unstable.step();
    poll_hook=poll_unstable; unstable.step();
    assert(unstable.tx.active && !finish_count && tx_status_reads==2);
    poll_hook=nullptr; tx_done(); unstable.step(); assert(finish_count==1 && status.ready);
}
static void test_tx_fairness_and_expiry() {
    reset(); radio_worker w; ready_state(w,0); ready_state(w,1); tx_busy(59);
    w.step(); assert(w.tx.id==1 && w.next_device==0);
    const auto deadline=w.tx.deadline_us;
    w.controls[1].deadline_ms=test_now_us/1000+2;
    test_now_us+=2000; w.step();
    assert(registry.records[1].control.phase==RF_CONTROL_TIMEOUT && !registry.records[1].command_reserved);
    assert(!registry.records[1].control.attempts && w.tx.deadline_us==deadline && transmit_count==1);
    tx_done(); w.step(); assert(w.next_device==1 && registry.records[0].control.attempts==1);
    // Fresh independent request may arrive while A is physically outstanding.
    reset(); radio_worker parallel; start_control(parallel,0); tx_busy(); parallel.step();

    assert(control_start_at(2,&command,test_now_us/1000,222)==ESP_OK);
    for (unsigned i=0;i<3;++i) parallel.step();
    assert(queue_count==1 && registry.records[1].command_pending && transmit_count==1);
    tx_done(); parallel.step(); assert(transmit_count==1 && parallel.next_device==1);
    parallel.step(); assert(parallel.tx.id==2 && transmit_count==2);
    parallel.step();
    assert(registry.records[0].control.phase==RF_CONTROL_WAIT_REPORT && registry.records[1].control.phase==RF_CONTROL_WAIT_REPORT);
    assert(registry.records[1].result_cookie==222 && registry.records[1].tracked_result.phase==RF_CONTROL_WAIT_REPORT);
    // A queued command expires from its admission time, rather than getting a
    // fresh allowance after waiting behind another transmission.
    reset(); test_now_us=200000000; radio_worker queued; start_control(queued,0); tx_busy(); queued.step();
    assert(control_start_at(2,&command,test_now_us/1000-RF_CONTROL_WAIT_MS+1,333)==ESP_OK);
    test_now_us+=2000; queued.step(); assert(registry.records[1].command_pending);
    tx_done(); queued.step(); queued.step();
    assert(registry.records[1].control.phase==RF_CONTROL_TIMEOUT && !registry.records[1].command_reserved);
    assert(registry.records[1].result_cookie==333 && transmit_count==1);
}
static void test_tx_local_queue_and_identity() {
    reset(); radio_worker w;
    assert(control_start_at(2,&command,test_now_us/1000,987)==ESP_OK); tx_busy(); w.step();
    assert(radio_device_remove(2)==ESP_ERR_INVALID_STATE);
    assert(radio_device_remove(1)==ESP_OK); // Compaction accepted during physical TX.
    assert(radio_mode_set(false)==RADIO_ERR_MODE_BUSY);
    for (unsigned i=0;i<10;++i) { test_now_us+=1000; w.step(); }
    assert(registry.count==2 && queue_count==1 && status.mutation_pending && !store_calls);
    assert(w.tx.id==2 && registry.records[1].result_cookie==987 && registry.records[1].command_reserved);
    tx_done(); w.step();
    assert(!tx_in_progress && registry.count==2 && queue_count==1 && !store_calls);
    assert(registry.records[1].tracked_result.phase==RF_CONTROL_WAIT_REPORT);
    w.step();
    assert(store_calls==1 && last_stored_id==1 && registry.count==1 && registry.records[0].id==2);
    assert(w.controls[0].radiator[3]==7 && registry.records[0].result_cookie==987);
    hear(w,report_b(false)); hear(w,report_b(true));
    assert(registry.records[0].tracked_result.phase==RF_CONTROL_MATCHED && registry.records[0].tracked_result.attempts==1);
    // Pending flash requests remain pending through faults and keep real outcomes.
    reset(); radio_worker failed; ready_state(failed,1); tx_busy(59); failed.step();
    assert(radio_device_rename(1,"Failed rename")==ESP_OK);
    test_now_us=failed.tx.deadline_us; failed.step();
    assert(!status.ready && queue_count==1 && status.mutation_pending && !store_calls);
    store_result=ESP_ERR_TIMEOUT; failed.step();
    assert(store_calls==1 && last_stored_id==1 && !status.mutation_pending);
    assert(status.mutation_error==ESP_ERR_TIMEOUT && strcmp(registry.records[0].name,"Failed rename"));
    // Exhaust the existing finite recovery batch, then explicitly retry. None
    // of these setup/retry iterations can revive the failed request or its bytes.
    begin_result=-99;
    for (unsigned i=0;i<RADIO_RECOVERY_ATTEMPTS;++i) {
        test_now_us=status.retry_at_ms*1000; failed.step();
    }
    assert(status.health==RADIO_FAULTED && transmit_count==1 && !tx_in_progress);
    assert(radio_retry()==ESP_OK); begin_result=0; failed.step(); failed.step();
    assert(status.ready && transmit_count==1 && registry.records[1].control.phase==RF_CONTROL_ERROR);
    store_result=0; assert(radio_device_rename(1,"After retry")==ESP_OK); failed.step();
    assert(store_calls==2 && !strcmp(registry.records[0].name,"After retry"));
    // Local write ahead of a command runs first; once admitted, the later
    // command cannot overtake or discard a pending store.
    reset(); radio_worker ordered; assert(radio_device_rename(1,"First")==ESP_OK);
    ordered.step(); assert(store_calls==1 && !transmit_count);
    assert(radio_control_start(2,&command)==ESP_OK);
    tx_busy(); ordered.step(); assert(radio_device_rename(1,"Second")==ESP_OK);
    ordered.step(); assert(store_calls==1 && queue_count==1);
    tx_done(); ordered.step(); ordered.step();
    assert(store_calls==2 && !strcmp(registry.records[0].name,"Second"));
}
static void test_tx_local_mutation() {
    reset(); radio_worker w;
    assert(control_start_at(2,&command,test_now_us/1000,456)==ESP_OK);
    w.step(); w.step();
    rf_frame report=report_b(false); rf_control_receive(&w.controls[1],test_now_us/1000,&report);
    tx_busy(59); w.step(); const auto pending=w.tx;
    assert(radio_device_rename(2,"After TX")==ESP_OK);
    assert(status.mutation_pending);
    w.step(); assert(!store_calls && queue_count==1 && tx_in_progress);
    assert(!memcmp(pending.bytes,w.tx.bytes,pending.size));
    tx_done(); w.step();
    assert(!store_calls && registry.records[1].tracked_result.attempts==1);
    w.step();
    assert(store_calls==1 && !status.mutation_error && !strcmp(registry.records[1].name,"After TX"));
    assert(registry.records[1].result_cookie==456 && registry.records[1].command_reserved);
    assert(w.controls[1].status.phase==RF_CONTROL_WAIT_READBACK && w.controls[1].status.attempts==1);
    hear(w,report_b(true));
    assert(registry.records[1].tracked_result.phase==RF_CONTROL_MATCHED && registry.records[1].result_cookie==456);
}
static void test_tx_pair_cancel_and_silence() {
    reset(); radio_worker w; const uint8_t c[]={0xf1,1,2,3},dst[]={0xf1,0x10,0,1}; uint32_t id;
    assert(radio_pair_start(c,dst,&id)==ESP_OK); tx_busy(); w.step();
    assert(tx_in_progress && w.tx.pairing && pair_reserved);
    assert(radio_pair_cancel(id)==ESP_OK && radio_mode_set(false)==RADIO_ERR_MODE_BUSY);
    w.step(); assert(pair_reserved && queue_count==1 && w.pair.phase==RF_PAIR_OFFER);
    tx_done(); w.step(); assert(w.pair.phase==RF_PAIR_WAIT_REPLY && pair_reserved);
    w.step(); assert(status.pairing.phase==RF_PAIR_CANCELLED && !pair_reserved && !exchange_id);
    assert(radio_mode_set(false)==ESP_OK); w.step();
    unsigned sent=transmit_count;
    w.fail(-101); begin_result=-102;
    for (unsigned i=0;i<RADIO_RECOVERY_ATTEMPTS;++i) { test_now_us=status.retry_at_ms*1000; w.step(); }
    assert(status.health==RADIO_FAULTED && radio_retry()==ESP_OK); begin_result=0; w.step();
    assert(status.ready && !status.control_enabled && transmit_count==sent && !tx_in_progress);
    // Simulated reboot creates new owner/machines; stored configuration creates
    // no outstanding physical work, including in listen-only.
    radio_transport_start(); radio_worker reboot; reboot.step(); reboot.step();
    assert(!reboot.tx.active && !tx_in_progress && transmit_count==sent);
    reset(); status.control_enabled=false; radio_worker silent;
    for (unsigned i=0;i<20;++i) { silent.step(); test_now_us+=1000; }
    silent.fail(-103); test_now_us=status.retry_at_ms*1000; silent.step(); silent.step();
    assert(status.ready && !transmit_count && !silent.tx.active && !tx_in_progress);
}
