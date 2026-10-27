// SPDX-License-Identifier: MIT
#include "idf_hal.h"
#include "radio_transport.h"
#include "esp_log.h"

static IdfHal hal;
static Module module(&hal, RADIO_CS, RADIO_GDO0, RADIOLIB_NC, RADIO_GDO2);
static CC1101 radio(&module);
static radio_status status = {false, 0, -1};

extern "C" radio_status radio_transport_status(void) { return status; }

extern "C" void radio_transport_start(void)
{
    // Working receive assumptions from the RF spec, not a calibrated TX profile.
    int result = radio.begin(868.3, 1000.0 / 27.0, 17.0, 101.6, 0, 32);
    status.chip_version = result == RADIOLIB_ERR_NONE ? radio.getChipVersion() : -1;
    if (result == RADIOLIB_ERR_NONE) result = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    if (result == RADIOLIB_ERR_NONE) result = radio.setSyncWord(0xd3, 0x91);
    // Require both repeated sync words (D391 D391), with no bit errors.
    if (result == RADIOLIB_ERR_NONE)
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG2, 3, 2, 0);
    if (result == RADIOLIB_ERR_NONE) result = radio.setCrcFiltering(false);
    if (result == RADIOLIB_ERR_NONE) result = radio.disableAddressFiltering();
    status.error = result;
    status.ready = result == RADIOLIB_ERR_NONE;
    ESP_LOGI("radio", "CC1101 version=%d status=%d", status.chip_version, result);
}
