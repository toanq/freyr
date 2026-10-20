#pragma once

class SynoraBoard;
class AudioCodec;

// Interactive REPL on the native USB-Serial-JTAG peripheral (/dev/ttyACM0 --
// the same port already used for flashing and log monitoring; see
// test_console.cc's InitializeTestConsole() comment for why UART0 isn't used
// instead), so test actions can be triggered on demand from a terminal
// (minicom/idf.py monitor) instead of waiting on their normal timer/event
// triggers. Debug-only tooling, no build guard -- esp_console's flash/RAM
// cost is negligible and this device has no untrusted physical-access
// threat model.
//
// codec must already be Start()ed with input+output enabled (see main.cc) --
// needed by the "mictest" command (record N seconds of mic input, play it
// straight back), which is otherwise independent of SynoraBoard.
void InitializeTestConsole(SynoraBoard* board, AudioCodec* codec);

// True while "mictest" is actively recording. esp-idf's I2S driver doesn't
// support two tasks calling i2s_channel_read() on the same RX channel
// concurrently -- confirmed live: it throws "channel is not enabled" (a
// state-machine error, not an actual disable) when both AudioInputTask and
// mictest's own read loop call it at once. AudioInputTask (main.cc) must
// check this and skip its own InputData() call while true.
bool IsMicTestRecording();
