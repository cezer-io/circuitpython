// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2016 Scott Shawcroft
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "hpl_sercom_config.h"
#include "peripheral_clk_config.h"

#include "supervisor/board.h"
#include "common-hal/busio/__init__.h"

#include "hal/include/hal_gpio.h"
#include "hal/include/hal_spi_m_sync.h"

#include "samd/dma.h"
#include "samd/sercom.h"

void setup_pin(const mcu_pin_obj_t *pin, uint32_t pinmux);

void common_hal_busio_spi_construct(busio_spi_obj_t *self,
    const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, bool half_duplex) {
    Sercom *sercom = NULL;
    uint8_t sercom_index;
    uint32_t clock_pinmux = 0;
    bool mosi_none = mosi == NULL;
    bool miso_none = miso == NULL;
    uint32_t mosi_pinmux = 0;
    uint32_t miso_pinmux = 0;
    uint8_t clock_pad = 0;
    uint8_t mosi_pad = 0;
    uint8_t miso_pad = 0;
    uint8_t dopo = 255;

    if (half_duplex) {
        mp_raise_NotImplementedError_varg(MP_ERROR_TEXT("%q"), MP_QSTR_half_duplex);
    }

    // Ensure the object starts in its deinit state.
    self->clock_pin = NO_PIN;

    // Special case for SAMR21 boards. (feather_radiofruit_zigbee)
    #if defined(PIN_PC19F_SERCOM4_PAD0)
    if (miso == &pin_PC19) {
        if (mosi == &pin_PB30 && clock == &pin_PC18) {
            sercom = SERCOM4;
            sercom_index = 4;
            clock_pinmux = MUX_F;
            mosi_pinmux = MUX_F;
            miso_pinmux = MUX_F;
            clock_pad = 3;
            mosi_pad = 2;
            miso_pad = 0;
            dopo = samd_peripherals_get_spi_dopo(clock_pad, mosi_pad);
        }
        // Error, leave SERCOM unset to throw an exception later.
    } else
    #endif
    {
        for (int i = 0; i < NUM_SERCOMS_PER_PIN; i++) {
            sercom_index = clock->sercom[i].index; // 2 for SERCOM2, etc.
            if (sercom_index >= SERCOM_INST_NUM) {
                continue;
            }
            Sercom *potential_sercom = sercom_insts[sercom_index];
            if (potential_sercom->SPI.CTRLA.bit.ENABLE != 0) {
                continue;
            }
            clock_pinmux = PINMUX(clock->number, (i == 0) ? MUX_C : MUX_D);
            clock_pad = clock->sercom[i].pad;
            if (!samd_peripherals_valid_spi_clock_pad(clock_pad)) {
                continue;
            }
            // find mosi_pad first, since it corresponds to dopo which takes limited values
            for (int j = 0; j < NUM_SERCOMS_PER_PIN; j++) {
                if (!mosi_none) {
                    if (sercom_index == mosi->sercom[j].index) {
                        mosi_pinmux = PINMUX(mosi->number, (j == 0) ? MUX_C : MUX_D);
                        mosi_pad = mosi->sercom[j].pad;
                        dopo = samd_peripherals_get_spi_dopo(clock_pad, mosi_pad);
                        if (dopo > 0x3) {
                            continue;  // pad combination not possible
                        }
                        if (miso_none) {
                            sercom = potential_sercom;
                            break;
                        }
                    } else {
                        continue;
                    }
                }
                if (!miso_none) {
                    for (int k = 0; k < NUM_SERCOMS_PER_PIN; k++) {
                        if (sercom_index == miso->sercom[k].index) {
                            miso_pinmux = PINMUX(miso->number, (k == 0) ? MUX_C : MUX_D);
                            miso_pad = miso->sercom[k].pad;
                            sercom = potential_sercom;
                            break;
                        }
                    }
                }
                if (sercom != NULL) {
                    break;
                }
            }
            if (sercom != NULL) {
                break;
            }
        }
    }
    if (sercom == NULL) {
        raise_ValueError_invalid_pins();
    }

    // Set up SPI clocks on SERCOM.
    samd_peripherals_sercom_clock_init(sercom, sercom_index);

    if (spi_m_sync_init(&self->spi_desc, sercom) != ERR_NONE) {
        mp_raise_OSError(MP_EIO);
    }

    // Pads must be set after spi_m_sync_init(), which uses default values from
    // the prototypical SERCOM.

    hri_sercomspi_write_CTRLA_MODE_bf(sercom, 3);
    hri_sercomspi_write_CTRLA_DOPO_bf(sercom, dopo);
    hri_sercomspi_write_CTRLA_DIPO_bf(sercom, miso_pad);

    // Always start at 250khz which is what SD cards need. They are sensitive to
    // SPI bus noise before they are put into SPI mode.
    uint8_t baud_value = samd_peripherals_spi_baudrate_to_baud_reg_value(250000);
    if (spi_m_sync_set_baudrate(&self->spi_desc, baud_value) != ERR_NONE) {
        // spi_m_sync_set_baudrate does not check for validity, just whether the device is
        // busy or not
        mp_raise_OSError(MP_EIO);
    }

    setup_pin(clock, clock_pinmux);
    self->clock_pin = clock->number;

    if (mosi_none) {
        self->MOSI_pin = NO_PIN;
    } else {
        setup_pin(mosi, mosi_pinmux);
        self->MOSI_pin = mosi->number;
    }

    if (miso_none) {
        self->MISO_pin = NO_PIN;
    } else {
        setup_pin(miso, miso_pinmux);
        self->MISO_pin = miso->number;
    }

    spi_m_sync_enable(&self->spi_desc);
}

void common_hal_busio_spi_never_reset(busio_spi_obj_t *self) {
    never_reset_sercom(self->spi_desc.dev.prvt);

    never_reset_pin_number(self->clock_pin);
    never_reset_pin_number(self->MOSI_pin);
    never_reset_pin_number(self->MISO_pin);
}

bool common_hal_busio_spi_deinited(busio_spi_obj_t *self) {
    return self->clock_pin == NO_PIN;
}

void common_hal_busio_spi_deinit(busio_spi_obj_t *self) {
    if (common_hal_busio_spi_deinited(self)) {
        return;
    }
    allow_reset_sercom(self->spi_desc.dev.prvt);

    spi_m_sync_disable(&self->spi_desc);
    spi_m_sync_deinit(&self->spi_desc);
    reset_pin_number(self->clock_pin);
    reset_pin_number(self->MOSI_pin);
    reset_pin_number(self->MISO_pin);
    self->clock_pin = NO_PIN;
}

bool common_hal_busio_spi_configure(busio_spi_obj_t *self,
    uint32_t baudrate, uint8_t polarity, uint8_t phase, uint8_t bits) {
    uint8_t baud_reg_value = samd_peripherals_spi_baudrate_to_baud_reg_value(baudrate);

    void *hw = self->spi_desc.dev.prvt;
    // If the settings are already what we want then don't reset them.
    if (hri_sercomspi_get_CTRLA_CPHA_bit(hw) == phase &&
        hri_sercomspi_get_CTRLA_CPOL_bit(hw) == polarity &&
        hri_sercomspi_read_CTRLB_CHSIZE_bf(hw) == ((uint32_t)bits - 8) &&
        hri_sercomspi_read_BAUD_BAUD_bf(hw) == baud_reg_value) {
        return true;
    }

    // Disable, set values (most or all are enable-protected), and re-enable.
    spi_m_sync_disable(&self->spi_desc);
    hri_sercomspi_wait_for_sync(hw, SERCOM_SPI_SYNCBUSY_MASK);

    hri_sercomspi_write_CTRLA_CPHA_bit(hw, phase);
    hri_sercomspi_write_CTRLA_CPOL_bit(hw, polarity);
    hri_sercomspi_write_CTRLB_CHSIZE_bf(hw, bits - 8);
    hri_sercomspi_write_BAUD_BAUD_bf(hw, baud_reg_value);
    hri_sercomspi_wait_for_sync(hw, SERCOM_SPI_SYNCBUSY_MASK);

    spi_m_sync_enable(&self->spi_desc);
    hri_sercomspi_wait_for_sync(hw, SERCOM_SPI_SYNCBUSY_MASK);

    return true;
}

bool common_hal_busio_spi_try_lock(busio_spi_obj_t *self) {
    if (common_hal_busio_spi_deinited(self)) {
        return false;
    }
    bool grabbed_lock = false;
    CRITICAL_SECTION_ENTER()
    if (!self->has_lock) {
        grabbed_lock = true;
        self->has_lock = true;
    }
    CRITICAL_SECTION_LEAVE();
    return grabbed_lock;
}

bool common_hal_busio_spi_has_lock(busio_spi_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_spi_unlock(busio_spi_obj_t *self) {
    self->has_lock = false;
}

bool common_hal_busio_spi_write(busio_spi_obj_t *self,
    const uint8_t *data, size_t len) {
    if (len == 0) {
        return true;
    }
    int32_t status;
    if (len >= 16) {
        size_t bytes_remaining = len;

        // Maximum DMA transfer is 65535
        while (1) {
            size_t to_send = (bytes_remaining > 65535) ? 65535 : bytes_remaining;
            status = sercom_dma_write(self->spi_desc.dev.prvt, data + (len - bytes_remaining), to_send);
            bytes_remaining -= to_send;
            if (bytes_remaining > 0) {
                // Multi-part transfer; let other things run before doing the next chunk.
                RUN_BACKGROUND_TASKS;
            } else {
                // All done.
                break;
            }
        }
    } else {
        struct io_descriptor *spi_io;
        spi_m_sync_get_io_descriptor(&self->spi_desc, &spi_io);
        status = spi_io->write(spi_io, data, len);
    }
    return status >= 0; // Status is number of chars read or an error code < 0.
}

bool common_hal_busio_spi_read(busio_spi_obj_t *self,
    uint8_t *data, size_t len, uint8_t write_value) {
    if (len == 0) {
        return true;
    }
    int32_t status;
    if (len >= 16) {
        status = sercom_dma_read(self->spi_desc.dev.prvt, data, len, write_value);
    } else {
        self->spi_desc.dev.dummy_byte = write_value;

        struct io_descriptor *spi_io;
        spi_m_sync_get_io_descriptor(&self->spi_desc, &spi_io);

        status = spi_io->read(spi_io, data, len);
    }
    return status >= 0; // Status is number of chars read or an error code < 0.
}

bool common_hal_busio_spi_transfer(busio_spi_obj_t *self, const uint8_t *data_out, uint8_t *data_in, size_t len) {
    if (len == 0) {
        return true;
    }
    int32_t status;
    if (len >= 16) {
        status = sercom_dma_transfer(self->spi_desc.dev.prvt, data_out, data_in, len);
    } else {
        struct spi_xfer xfer;
        xfer.txbuf = (uint8_t *)data_out;
        xfer.rxbuf = data_in;
        xfer.size = len;
        status = spi_m_sync_transfer(&self->spi_desc, &xfer);
    }
    return status >= 0; // Status is number of chars read or an error code < 0.
}

uint32_t common_hal_busio_spi_get_frequency(busio_spi_obj_t *self) {
    return samd_peripherals_spi_baud_reg_value_to_baudrate(hri_sercomspi_read_BAUD_reg(self->spi_desc.dev.prvt));
}

uint8_t common_hal_busio_spi_get_phase(busio_spi_obj_t *self) {
    void *hw = self->spi_desc.dev.prvt;
    return hri_sercomspi_get_CTRLA_CPHA_bit(hw);
}

uint8_t common_hal_busio_spi_get_polarity(busio_spi_obj_t *self) {
    void *hw = self->spi_desc.dev.prvt;
    return hri_sercomspi_get_CTRLA_CPOL_bit(hw);
}

void setup_pin(const mcu_pin_obj_t *pin, uint32_t pinmux) {
    gpio_set_pin_direction(pin->number, GPIO_DIRECTION_OUT);
    gpio_set_pin_pull_mode(pin->number, GPIO_PULL_OFF);
    gpio_set_pin_function(pin->number, pinmux);
    claim_pin(pin);
    hri_port_set_PINCFG_DRVSTR_bit(PORT, (enum gpio_port)GPIO_PORT(pin->number), GPIO_PIN(pin->number));
}
