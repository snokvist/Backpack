#pragma once
#include <Arduino.h>

// Waybeam SuperMini TX-Backpack host-channel fork.
//
// Stock Backpack uses `Serial` (UART0) as the CRSF/MSP link to the ELRS TX module.
// The Waybeam host-channel env (HOST_USB_CDC) claims USB-CDC for a framed host
// protocol and moves the ELRS link to UART1 on configurable pins. `ELRS_SERIAL`
// lets existing call sites stay readable while the redirection happens once.

#ifdef HOST_USB_CDC
  #define ELRS_SERIAL Serial1
#else
  #define ELRS_SERIAL Serial
#endif
