// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "common-hal/analogio/AnalogIn.h"
#include "shared-bindings/analogio/AnalogIn.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "py/runtime.h"

#include "hardware/adc.h"

// On many boards with a CYW43 radio co-processor, CYW43_DEFAULT_PIN_WL_CLOCK (usually GPIO29),
// is both a voltage monitor and also SPI SCK to the CYW43.
// Special handling is required to read the analog voltage.
#if CIRCUITPY_CYW43
#include "bindings/cyw43/__init__.h"
#define SPECIAL_PIN(pin) (pin->number == CYW43_DEFAULT_PIN_WL_CLOCK)

const mcu_pin_obj_t *common_hal_analogio_analogin_validate_pin(mp_obj_t obj) {
    return validate_obj_is_free_pin_or_gpio29(obj, MP_QSTR_pin);
}
#else
#define SPECIAL_PIN(pin) false
#endif

void common_hal_analogio_analogin_construct(analogio_analogin_obj_t *self, const mcu_pin_obj_t *pin) {
    if (pin->number < ADC_BASE_PIN || pin->number > ADC_BASE_PIN + NUM_ADC_CHANNELS - 1) {
        raise_ValueError_invalid_pin();
    }

    adc_init();
    if (!SPECIAL_PIN(pin)) {
        adc_gpio_init(pin->number);
        claim_pin(pin);
    }

    self->pin = pin;
}

bool common_hal_analogio_analogin_deinited(analogio_analogin_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_analogio_analogin_deinit(analogio_analogin_obj_t *self) {
    if (common_hal_analogio_analogin_deinited(self)) {
        return;
    }

    if (!SPECIAL_PIN(self->pin)) {
        reset_pin_number(self->pin->number);
    }
    self->pin = NULL;
}

uint16_t common_hal_analogio_analogin_get_value(analogio_analogin_obj_t *self) {
    uint16_t value;
    if (SPECIAL_PIN(self->pin)) {
        common_hal_mcu_disable_interrupts();
        uint32_t old_pad = pads_bank0_hw->io[self->pin->number];
        uint32_t old_ctrl = io_bank0_hw->io[self->pin->number].ctrl;
        adc_gpio_init(self->pin->number);
        adc_select_input(self->pin->number - ADC_BASE_PIN);
        common_hal_mcu_delay_us(100);
        value = adc_read();
        gpio_init(self->pin->number);
        pads_bank0_hw->io[self->pin->number] = old_pad;
        io_bank0_hw->io[self->pin->number].ctrl = old_ctrl;
        common_hal_mcu_enable_interrupts();
    } else {
        adc_select_input(self->pin->number - ADC_BASE_PIN);
        value = adc_read();
    }
    // Stretch 12-bit ADC reading to 16-bit range
    return (value << 4) | (value >> 8);
}

float common_hal_analogio_analogin_get_reference_voltage(analogio_analogin_obj_t *self) {
    // The nominal VCC voltage
    return 3.3f;
}
