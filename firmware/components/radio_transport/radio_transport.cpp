// SPDX-License-Identifier: MIT
// The radio task owns the CC1101. It receives continuously and runs at most
// one RF exchange at a time: either a pairing or one queued command.
#ifdef CLEVERBOX_HOST
#include "platform.h"
#else
#include <RadioLib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "board.h"
static_assert(RADIOLIB_VERSION == 0x07080100, "Expected pinned RadioLib 7.8.1");
#endif
#include "radio_transport.h"
#include "device_time.h"
#include "esp_log.h"
#include <string.h>

static EspHal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module module(&hal, RADIO_CS, RADIO_GDO0, RADIOLIB_NC, RADIO_GDO2);
static CC1101 radio(&module);

// Everything below is shared with HTTP/MQTT callers and guarded by `lock`.
// SPI and radio calls happen outside it; NVS writes happen inside it.
static SemaphoreHandle_t lock;
static radio_status status;
static radiator_registry registry;
static radio_observation observations[RADIO_OBSERVATION_CAPACITY];
static rf_control control;   // The running command, if control_id != 0.
static uint32_t control_id;
static size_t observation_count;
static uint32_t queue[CLEVERBOX_RADIATOR_CAPACITY]; // Queued command IDs, oldest first.
static size_t queued;
static bool retry_requested;

// Receive state, owned by the radio task.
static rf_frame rx_frame;
static int64_t rx_started = -1;

static void take() { xSemaphoreTake(lock, portMAX_DELAY); }
static void give() { xSemaphoreGive(lock); }
static int64_t now_ms() { return esp_timer_get_time() / 1000; }

extern "C" const char *radio_error_name(int e)
{
    switch (e) {
    case ESP_OK: return "none";
    case RADIO_ERR_NOT_READY: return "radio_not_ready";
    case RADIO_ERR_BUSY: return "busy";
    case RADIO_ERR_TIME_UNAVAILABLE: return "time_unavailable";
    case RADIO_ERR_FULL: return "registry_full";
    case RADIO_ERR_DUPLICATE: return "already_registered";
    case RADIO_ERR_NOT_PAIRED: return "not_paired";
    case ESP_ERR_INVALID_ARG: return "invalid_request";
    case ESP_ERR_NOT_FOUND: return "unknown_radiator";
    default: return "error";
    }
}

static radiator_record *find(uint32_t id)
{
    for (size_t i = 0; i < registry.count; ++i)
        if (registry.records[i].id == id) return &registry.records[i];
    return nullptr;
}

static radiator_record *find_radiator(const uint8_t address[4])
{
    for (size_t i = 0; i < registry.count; ++i)
        if (!memcmp(registry.records[i].radiator, address, 4)) return &registry.records[i];
    return nullptr;
}

static esp_err_t save_registry()
{
    ++status.registry_version;
    status.registry_error = registry_save(&registry);
    return status.registry_error;
}

// Latest report per source address. A full cache replaces the oldest entry.
static void observe(const rf_report &report, int64_t now)
{
    size_t slot = 0;
    while (slot < observation_count && memcmp(observations[slot].report.source, report.source, 4)) ++slot;
    if (slot == RADIO_OBSERVATION_CAPACITY) {
        slot = 0;
        for (size_t i = 1; i < RADIO_OBSERVATION_CAPACITY; ++i)
            if (observations[i].time_ms < observations[slot].time_ms) slot = i;
    } else if (slot == observation_count) ++observation_count;
    observations[slot] = {report, now};
}

static void add_paired_radiator()
{
    const rf_pairing &p = status.pairing;
    if (registry.count == CLEVERBOX_RADIATOR_CAPACITY) {
        status.pairing.phase = RF_PAIR_ERROR; status.pairing_error = RADIO_ERR_FULL; return;
    }
    radiator_record &d = registry.records[registry.count++];
    d = {};
    d.id = status.pairing_id;
    strcpy(d.name, "Radiator");
    memcpy(d.controller, p.controller, 4);
    memcpy(d.radiator, p.radiator, 4);
    status.pairing_error = save_registry();
}

// Called for each complete frame. Routes reports to records and machines.
static void handle_frame(const rf_frame &f, int64_t now)
{
    take();
    if (!rf_valid(&f)) { ++status.invalid_frames; give(); return; }
    ++status.valid_frames;
    status.has_frame = true; status.last_frame = f; status.frame_ms = now;

    rf_pairing &p = status.pairing;
    rf_pair_phase before = p.phase;
    rf_pair_receive(&p, now, &f);
    if (before == RF_PAIR_WAIT_REPLY && p.phase == RF_PAIR_BIND && find_radiator(p.radiator)) {
        p.phase = RF_PAIR_ERROR; status.pairing_error = RADIO_ERR_DUPLICATE; // Before bind is sent.
    }
    if (before != RF_PAIR_COMPLETE && p.phase == RF_PAIR_COMPLETE) add_paired_radiator();

    rf_report report;
    if (rf_decode_report(&f, &report)) {
        observe(report, now);
        for (size_t i = 0; i < registry.count; ++i) {
            radiator_record &d = registry.records[i];
            bool paired = radiator_paired(&d);
            // Paired records need both addresses; monitored ones accept any controller.
            if (memcmp(report.source, d.radiator, 4) || (paired && memcmp(report.destination, d.controller, 4))) continue;
            d.has_report = true; d.report = report; d.report_ms = now;
            if (!paired) continue;
            if (control_id == d.id) {
                rf_control_receive(&control, now, &f);
                d.command = control.status;
            } else if ((d.command.phase == RF_CONTROL_TIMEOUT || d.command.phase == RF_CONTROL_ERROR) &&
                       rf_control_reconcile(&d.command, now, &f)) {
                d.command_error = 0; // A later report showed the requested settings.
            }
        }
    }
    give();
}

static void fail(const char *stage, int error)
{
    take();
    status.ready = false; status.error = error; status.error_stage = stage;
    if (rf_pair_active(&status.pairing)) { status.pairing.phase = RF_PAIR_ERROR; status.pairing_error = error; }
    if (control_id) {
        control.status.phase = RF_CONTROL_ERROR;
        if (radiator_record *d = find(control_id)) { d->command = control.status; d->command_error = error; }
        control_id = 0;
    }
    for (size_t i = 0; i < queued; ++i)
        if (radiator_record *d = find(queue[i])) {
            d->command_queued = false; d->command.phase = RF_CONTROL_ERROR; d->command_error = RADIO_ERR_NOT_READY;
        }
    queued = 0;
    give();
    rx_frame = {}; rx_started = -1;
    ESP_LOGE("radio", "%s failed: %d; use retry or restart", stage, error);
}

static void initialize()
{
    // RF profile from the protocol specification; see docs/hardware.md.
    int r = radio.begin(868.3, 1000.0 / 27.0, 17.0, 101.6, 0, 32);
    int version = r ? -1 : radio.getChipVersion();
    hal.pinMode(RADIO_GDO2, hal.GpioModeInput); // RadioLib's transmit() polls GDO2.
    // Preamble length on receive is not established; do not require one.
    if (!r) r = radio.setPreambleLength(32, 0);
    if (!r) r = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    if (!r) r = radio.setSyncWord(0xd3, 0x91);
    // 30/32-bit sync detection for the repeated D391 D391 word.
    if (!r) r = module.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG2, 3, 2, 0);
    if (!r) r = radio.setCrcFiltering(false);
    if (!r) r = radio.disableAddressFiltering();
    // Infinite packet mode keeps the length byte and both software CRCs in the
    // FIFO; variable-length hardware mode would stop before the outer CRC.
    if (!r) r = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
    if (!r) r = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL1, 0, 2, 2);
    if (!r) r = radio.startReceive();
    rx_frame = {}; rx_started = -1;
    take();
    status.chip_version = version;
    status.ready = r == 0;
    if (r) { status.error = r; status.error_stage = "init"; }
    give();
    if (r) ESP_LOGE("radio", "CC1101 setup failed: %d", r);
    else ESP_LOGI("radio", "CC1101 version %d receiving", version);
}

// RXBYTES can be torn while the radio updates it (TI SWRZ020E). Accept a value
// only when two consecutive reads agree; otherwise try again next tick.
static bool rx_count(uint8_t &count)
{
    uint8_t previous = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
    for (int i = 0; i < 8; ++i) {
        count = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
        if (count == previous) return true;
        previous = count;
    }
    return false;
}

// Read what the FIFO holds. Frames are a length byte L (>= 11) plus L + 2
// bytes. Returns a RadioLib error, or 0.
static int receive(int64_t now)
{
    uint8_t count = 0;
    bool stable = rx_count(count);
    if ((!stable || count) && rx_started < 0) rx_started = now;
    bool restart = stable && (count & 0x80); // Overflow.
    bool error = restart;
    int result = 0;
    count &= 0x7f;
    if (stable && !restart && count) {
        if (count > 64) {
            restart = error = true;
        } else {
            // Never empty the FIFO mid-frame: reading the last byte while a new
            // one arrives can duplicate it (TI erratum).
            if (!rx_frame.size && count > 1) {
                rx_frame.bytes[0] = module.SPIreadRegister(RADIOLIB_CC1101_REG_FIFO);
                rx_frame.size = 1;
                --count;
            }
            size_t expected = (size_t)rx_frame.bytes[0] + 3;
            if (rx_frame.size && rx_frame.bytes[0] >= 11) {
                size_t remaining = expected - rx_frame.size;
                bool complete = count >= remaining;
                size_t take_bytes = complete ? remaining : (count > 1 ? count - 1 : 0);
                // Infinite mode has no packet end: stop RX before the final read.
                if (complete) result = radio.standby();
                if (!result && take_bytes) {
                    module.SPIreadRegisterBurst(RADIOLIB_CC1101_REG_FIFO | 0x40, take_bytes, rx_frame.bytes + rx_frame.size);
                    rx_frame.size += take_bytes;
                }
            }
            if (!result && rx_frame.size && (rx_frame.bytes[0] < 11 || rx_frame.size == expected)) {
                handle_frame(rx_frame, now);
                restart = true;
            }
        }
    }
    // A 258-byte frame takes about 56 ms at this bit rate.
    if (rx_started >= 0 && now - rx_started > 100) restart = error = true;
    if (restart) {
        if (!result) result = radio.startReceive(); // Standby, flush, RX.
        if (error) { take(); ++status.receive_errors; give(); }
        rx_frame = {}; rx_started = -1;
    }
    return result;
}

// Half-duplex check before sending. A frame can still start after it.
static bool rx_idle(int64_t now)
{
    uint8_t count = 0;
    if (rx_frame.size || rx_started >= 0) return false;
    if (rx_count(count) && !count) return true;
    rx_started = now;
    return false;
}

static int transmit(const rf_frame &f)
{
    int r = f.size <= 64 ? radio.standby() : ESP_ERR_INVALID_ARG;
    if (!r) r = radio.fixedPacketLengthMode(f.size);
    if (!r) r = radio.transmit(f.bytes, f.size); // Blocks until sent; bounded by RadioLib.
    // Back to infinite-length receive, also after a failed transmission.
    int rx = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
    if (!rx) rx = radio.startReceive();
    return r ? r : rx;
}

static void start_next_command(int64_t now)
{
    uint32_t id = queue[0];
    memmove(queue, queue + 1, --queued * sizeof(queue[0]));
    radiator_record *d = find(id);
    if (!d) return;
    d->command_queued = false;
    rf_control_request request = {d->command.mode, d->command.centi_c, 0};
    int64_t sampled;
    if (!device_time_command_clock(&request.clock_seconds, &sampled)) {
        d->command.phase = RF_CONTROL_ERROR; d->command_error = RADIO_ERR_TIME_UNAVAILABLE; return;
    }
    control = {};
    if (!rf_control_start(&control, &request, d->controller, d->radiator, now)) {
        d->command.phase = RF_CONTROL_ERROR; d->command_error = ESP_ERR_INVALID_ARG; return;
    }
    control_id = id;
    d->command = control.status;
}

enum tx_kind { TX_NONE, TX_PAIRING, TX_COMMAND };

// Advance deadlines, start the next queued command and choose a frame to send.
static tx_kind advance(int64_t now, rf_frame &out)
{
    rf_pair_tick(&status.pairing, now);
    if (control_id) {
        rf_control_tick(&control, now);
        if (radiator_record *d = find(control_id)) d->command = control.status;
        if (!rf_control_active(&control.status)) control_id = 0;
    }
    if (!control_id && !rf_pair_active(&status.pairing) && queued) start_next_command(now);
    if (rf_pair_next(&status.pairing, now, &out)) return TX_PAIRING;
    if (control_id && rf_control_next(&control, now, &out)) return TX_COMMAND;
    return TX_NONE;
}

static void sent(tx_kind kind, int error)
{
    int64_t now = now_ms();
    if (!error) ++status.transmissions;
    if (kind == TX_PAIRING) {
        if (error) status.pairing_error = error;
        rf_pair_sent(&status.pairing, now, !error);
    } else {
        rf_control_sent(&control, now, !error);
        if (radiator_record *d = find(control_id)) {
            d->command = control.status;
            if (error) d->command_error = error;
        }
    }
}

static void step()
{
    int64_t now = now_ms();
    take();
    bool retry = retry_requested, ready = status.ready;
    retry_requested = false;
    give();
    if (retry) { initialize(); return; }
    if (!ready) return;
    int e = receive(now);
    if (e) { fail("receive", e); return; }
    rf_frame out = {};
    take();
    tx_kind kind = advance(now, out);
    give();
    if (kind == TX_NONE || !rx_idle(now)) return;
    e = transmit(out);
    take(); sent(kind, e); give();
    if (e) fail("transmit", e);
}

static void radio_task(void *)
{
    initialize();
    for (;;) {
        step();
        vTaskDelay(1);
    }
}

extern "C" void radio_transport_start(void)
{
    lock = xSemaphoreCreateMutex();
    status.chip_version = -1;
    status.error_stage = "none";
    status.registry_error = registry_load(&registry);
    if (status.registry_error) ESP_LOGE("radio", "Registry load failed: %d", status.registry_error);
    xTaskCreate(radio_task, "radio", 6144, nullptr, 5, nullptr);
}

extern "C" radio_status radio_transport_status(void)
{
    take(); radio_status copy = status; give();
    return copy;
}

extern "C" size_t radio_radiators(radiator_record *out, size_t capacity)
{
    take();
    size_t n = registry.count < capacity ? registry.count : capacity;
    memcpy(out, registry.records, n * sizeof(*out));
    give();
    return n;
}

extern "C" size_t radio_observations(radio_observation *out, size_t capacity)
{
    take();
    size_t n = observation_count < capacity ? observation_count : capacity;
    memcpy(out, observations, n * sizeof(*out));
    give();
    return n;
}

extern "C" esp_err_t radio_retry(void)
{
    take();
    esp_err_t e = status.ready ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (!e) retry_requested = true;
    give();
    return e;
}

static bool busy() { return control_id || queued || rf_pair_active(&status.pairing); }

extern "C" esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t discovery[4], uint32_t *id)
{
    take();
    esp_err_t e = !status.ready ? RADIO_ERR_NOT_READY : busy() ? RADIO_ERR_BUSY :
                  registry.count == CLEVERBOX_RADIATOR_CAPACITY ? RADIO_ERR_FULL : ESP_OK;
    if (!e) {
        rf_pairing p = {};
        if (!rf_pair_start(&p, controller, discovery, now_ms())) e = ESP_ERR_INVALID_ARG;
        else {
            status.pairing = p; status.pairing_error = 0;
            // Reserve the ID now; IDs are never reused, so a failed attempt leaves a gap.
            status.pairing_id = *id = registry.next_id++;
        }
    }
    give();
    return e;
}

extern "C" esp_err_t radio_pair_cancel(void)
{
    take();
    esp_err_t e = rf_pair_active(&status.pairing) ? ESP_OK : ESP_ERR_INVALID_STATE;
    rf_pair_cancel(&status.pairing);
    give();
    return e;
}

extern "C" esp_err_t radio_command(uint32_t id, rf_mode mode, int centi_c, uint64_t request_id)
{
    rf_control check = {};
    rf_control_request request = {mode, centi_c, 0};
    take();
    radiator_record *d = find(id);
    unsigned clock; int64_t sampled;
    esp_err_t e = !d ? ESP_ERR_NOT_FOUND : !radiator_paired(d) ? RADIO_ERR_NOT_PAIRED :
        !rf_control_start(&check, &request, d->controller, d->radiator, 0) ? ESP_ERR_INVALID_ARG :
        !status.ready ? RADIO_ERR_NOT_READY :
        rf_pair_active(&status.pairing) || d->command_queued || control_id == id ? RADIO_ERR_BUSY :
        !device_time_command_clock(&clock, &sampled) ? RADIO_ERR_TIME_UNAVAILABLE : ESP_OK;
    if (!e) {
        d->command = check.status;
        d->command.phase = RF_CONTROL_IDLE;
        d->command_queued = true;
        d->command_error = 0;
        d->request_id = request_id;
        queue[queued++] = id;
    }
    give();
    return e;
}

extern "C" esp_err_t radio_rename(uint32_t id, const char *name)
{
    if (!radiator_name_valid(name)) return ESP_ERR_INVALID_ARG;
    take();
    radiator_record *d = find(id);
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (d) { strcpy(d->name, name); e = save_registry(); }
    give();
    return e;
}

extern "C" esp_err_t radio_remove(uint32_t id)
{
    take();
    radiator_record *d = find(id);
    esp_err_t e = !d ? ESP_ERR_NOT_FOUND : d->command_queued || control_id == id ? RADIO_ERR_BUSY : ESP_OK;
    if (!e) {
        size_t i = d - registry.records;
        memmove(d, d + 1, (registry.count - i - 1) * sizeof(*d));
        --registry.count;
        e = save_registry();
    }
    give();
    return e;
}

extern "C" esp_err_t radio_monitor(const uint8_t radiator[4], const char *name, uint32_t *id)
{
    if (!radiator_name_valid(name)) return ESP_ERR_INVALID_ARG;
    take();
    esp_err_t e = find_radiator(radiator) ? RADIO_ERR_DUPLICATE :
                  registry.count == CLEVERBOX_RADIATOR_CAPACITY ? RADIO_ERR_FULL : ESP_OK;
    if (!e) {
        radiator_record &d = registry.records[registry.count++];
        d = {};
        d.id = *id = registry.next_id++;
        strcpy(d.name, name);
        memcpy(d.radiator, radiator, 4);
        for (size_t i = 0; i < observation_count; ++i)
            if (!memcmp(observations[i].report.source, radiator, 4)) {
                d.has_report = true; d.report = observations[i].report; d.report_ms = observations[i].time_ms;
            }
        e = save_registry();
    }
    give();
    return e;
}
