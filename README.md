# PAW3395 driver implementation for ZMK

This an **experimental** input subsystem module, that grant ability to call a non-disclosed PAW3395 core static library.

> [!CAUTION]
> This module is build for nRF52840 Cortex-m4 MCU. Only tested on Xiao BLE.

## Installation

Include this project on ZMK's west manifest in `config/west.yml`:

```yml
manifest:
  remotes:
    ...
    # START #####
    - name: badjeff
      url-base: https://github.com/badjeff
    # END #######
    ...
  projects:
    ...
    # START #####
    - name: zmk-paw3395-driver
      remote: badjeff
      revision: main
    # END #######
    ...
  self:
    path: config
```

Update `board.overlay` adding the necessary bits (update the pins for your board accordingly):

```dts
&pinctrl {
	spi2_default: spi2_default {
		group1 {
			psels = <NRF_PSEL(SPIM_SCK, 1, 13)>,
			<NRF_PSEL(SPIM_MOSI, 1, 15)>,
			<NRF_PSEL(SPIM_MISO, 1, 14)>;
		};
	};
	spi2_sleep: spi2_sleep {
		group1 {
			psels = <NRF_PSEL(SPIM_SCK, 1, 13)>,
			<NRF_PSEL(SPIM_MOSI, 1, 15)>,
			<NRF_PSEL(SPIM_MISO, 1, 14)>;
			low-power-enable;
		};
	};
};

#include <zephyr/dt-bindings/input/input-event-codes.h>

&spi2 {
	compatible = "nordic,nrf-spim";
  status = "okay";
	pinctrl-0 = <&spi2_default>;
	pinctrl-1 = <&spi2_sleep>;
	pinctrl-names = "default", "sleep";
	cs-gpios = <&gpio1 12 GPIO_ACTIVE_LOW>;

	pd0: pd@0 {
		compatible = "pixart,paw3395";
	  status = "okay";
		label = "MOU0";
		reg = <0>;
		spi-max-frequency = <1500000>;

		// paw3395 driver parameters
		irq-gpios = <&gpio1 11 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
		cpi = <600>;
		evt-type = <INPUT_EV_REL>;
		x-input-code = <INPUT_REL_X>;
		y-input-code = <INPUT_REL_Y>;
	};
};

/ {
  mouse_input_listener {
    compatible = "zmk,input-listener";
    device = <&pd0>;
  };
};
```

Enable the driver config in `<shield>.config` file (read the Kconfig file to find out all possible options):

```conf
CONFIG_SPI=y
CONFIG_INPUT=y
CONFIG_ZMK_POINTING=y
CONFIG_PAW3395=y
# CONFIG_PAW3395_SWAP_XY=y
# CONFIG_PAW3395_INVERT_X=y
# CONFIG_PAW3395_INVERT_Y=y
# CONFIG_PAW3395_REPORT_INTERVAL_MIN=12
# CONFIG_PAW3395_LOG_LEVEL_DBG=y
# CONFIG_PAW3395_INIT_POWER_UP_EXTRA_DELAY_MS=3000
```
