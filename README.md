# ESP32 SIM800 AT COMMAND CONSOLE


## Overview
This is a terminal-broker application using an ESP32 as Data Termional Equipment (DTE) connected to a SIM800 GPRS modem (DCE). The ESP32 provides the console functionaity and fetures to connect to any terminal application using UART0 on the ESP32.

The application is an adaptation of two ESP32-IDF example applications 1) The ESP console component and 2) modem/mqtt component. I also used a snippet of code by David Mitzek for the cloudMyCar project with regards to the SIM800 odd behaviour.

The application sends all AT commands and prints their response. There are two commands `poweron` and `poweroff` that switch on and off the modem respectively.

## Why did I developed this Application
The Sim800 code provided by Espressif is good enough for amatuer projects but its not good for industrial  projects. There is a lot missing especially on capturing URC (Unsolisited Response Codes) in order to establish tyhe modem's state. I needed to experiment and see what was happening between the SIM800 and the ESP32. This app gave be the chance to send commands and see results in real time. As I progressed I inserted the URCs into the sim800 code so the component in this project is not compliant with the found in the ESP-IDF SDK.

## Harware Required
1) Any ESP32 based board with UART0 connect to a PC or Terminal and UART1 connected to
2) SIM_COMM 800 series GPRS modem. There is also code for BG96 whihc should work out of the box but have not been tested.

#### Pin Assignment

**Note:** The following pin assignments are used by default which can be changed in menuconfig.

| ESP32  | Cellular Modem |
| ------ | -------------- |
| GPIO26 | RX             |
| GPIO27 | TX             |
| GND    | GND            |
| GPIO23 | VCC            |** 
! GPIO4  | PWKEY          |
! GPIO5  ! RST            |
**This only is need of TTGO-CALL board. For other boards you can connect direct to VCC**

## How to use application

The application has to be compiled using ESP-IDF V4.0 SDK. To run either use the monitor feature in the idf.py or else just use Putty on Windows or Minicom on Linux to connect to the 

### Hardware Required

To run this example, you need an ESP32 dev board (e.g. ESP32-WROVER Kit) or ESP32 core board (e.g. ESP32-DevKitC).
For test purpose, you also need a cellular modem module. Here we take the [SIM800L](http://www.simcom.com/product/showproduct.php?lang=en&id=277) and [BG96](https://www.quectel.com/product/bg96.htm) as an example.
You can also try other modules as long as they embedded PPP protocol.

**Note:** Since SIM800L only support **2G** which will **not** work in some countries. And also keep in mind that in some other countries it will stop working soon (many remaining 2G networks will be switched off in the next 2-3 years). So you should **check with your local providers for further details** if you try this example with any 2G modules.


### Configure the project

Open the project configuration menu (`idf.py menuconfig`). Then go into `Console Configuration` and `Modem Configuration` menu.

- Choose the modem module in `Choose supported modem device(DCE)` option, currently we only support BG96 and SIM800L.
- Set the access point name in `Set Access Point Name(APN)` option, which should depend on the operator of your SIM card.
- Set the username and password for PPP authentication in `Set username for authentication` and `Set password for authentication` options.
- Select `Send MSG before power off` if you want to send a short message in the end of this example, and also you need to set the phone number correctly in `Peer Phone Number(with area code)` option.
- In `UART Configuration` menu, you need to set the GPIO numbers of UART and task specific parameters such as stack size, priority.

**Note:** During PPP setup, we should specify the way of authentication negotiation. By default it's configured to `PAP`. You can change to others (e.g. `CHAP`) in `Component config-->LWIP-->Enable PPP support` menu.

### Build and Flash

Run `idf.py -p PORT flash monitor` to build and flash the project..

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

## Troubleshooting
1. Why sending AT commands always failed and this example just keeping rebooting? e.g.

```bash
E (626) sim800: sim800_sync(293): send command failed
E (626) sim800: sim800_init(628): sync failed
```
   * Make sure your modem module is in command mode stably before you run this example.

(For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you as soon as possible.)
