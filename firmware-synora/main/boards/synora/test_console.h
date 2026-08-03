#pragma once

class SynoraBoard;

// Interactive REPL on the native USB-Serial-JTAG peripheral (/dev/ttyACM0 --
// the same port already used for flashing and log monitoring; see
// test_console.cc's InitializeTestConsole() comment for why UART0 isn't used
// instead), so test actions can be triggered on demand from a terminal
// (minicom/idf.py monitor) instead of waiting on their normal timer/event
// triggers. Debug-only tooling, no build guard -- esp_console's flash/RAM
// cost is negligible and this device has no untrusted physical-access
// threat model.
void InitializeTestConsole(SynoraBoard* board);
