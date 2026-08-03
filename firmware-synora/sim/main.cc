// Host-native click-to-talk simulator. Not a port of the firmware -- the
// real firmware can't run on Linux (ESP-IDF's Linux target has no BLE/WiFi/
// I2S/SPI/display, and there's no ESP32-S3 QEMU set up here either). This
// reuses the same pure-logic message building (voice_messages.cc) and talks
// to the real MQTT broker/server over a real TCP socket via libmosquitto,
// with a TUI standing in for the eyes display and a command prompt standing
// in for the BOOT button. No audio: there's no mic/speaker on a Linux host,
// so this only exercises the control-message half of the voice session
// (hello/listen start-stop, stt/llm/tts/system/alert) -- Opus encode/decode
// is ESP-IDF-only and already covered by the on-target build + host tests
// for its own message-building logic.
//
// Build/run: see README.md in this directory (needs libmosquitto-dev +
// libncurses-dev, and network access to the compose stack's `mosquitto`/
// `server` services -- run inside the same docker network).

#include "host_mqtt.h"
#include "../main/boards/synora/voice_messages.h"

#include <ncurses.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>

namespace {

enum class EyesState { kIdle, kListening, kTalking };

std::mutex g_state_mutex;
EyesState g_eyes_state = EyesState::kIdle;
std::deque<std::string> g_log_lines;
constexpr size_t kMaxLogLines = 200;

void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_log_lines.push_back(line);
    if (g_log_lines.size() > kMaxLogLines) {
        g_log_lines.pop_front();
    }
}

void SetEyes(EyesState state) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_eyes_state = state;
}

const char* EyesArt(EyesState state) {
    switch (state) {
        case EyesState::kIdle: return "( - . - )  idle (closed)";
        case EyesState::kListening: return "( o . o )  listening";
        case EyesState::kTalking: return "( O . O )  talking";
    }
    return "?";
}

// Runs a one-shot POST {server_url}/devices/activation via python3 (already
// proven to work against this stack's Traefik route, see this session's
// manual testing) -- not worth pulling in a whole HTTP client library for a
// single startup call. Returns the device's Guid id, or empty on failure.
std::string ActivateDevice(const std::string& server_url, const std::string& mac_address) {
    std::ostringstream cmd;
    cmd << "python3 -c \"\n"
        << "import urllib.request, json\n"
        << "req = urllib.request.Request('" << server_url << "/devices/activation',\n"
        << "    data=json.dumps({'macAddress': '" << mac_address << "'}).encode(),\n"
        << "    headers={'Content-Type': 'application/json'}, method='POST')\n"
        << "print(json.load(urllib.request.urlopen(req))['deviceId'])\n"
        << "\"";
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (pipe == nullptr) {
        return "";
    }
    char buf[128] = {};
    std::string result;
    if (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result = buf;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
    }
    pclose(pipe);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mqtt_host = std::getenv("SIM_MQTT_HOST") ? std::getenv("SIM_MQTT_HOST") : "mosquitto";
    int mqtt_port = std::getenv("SIM_MQTT_PORT") ? std::atoi(std::getenv("SIM_MQTT_PORT")) : 1883;
    std::string mqtt_user = std::getenv("SIM_MQTT_USERNAME") ? std::getenv("SIM_MQTT_USERNAME") : "synora_device";
    std::string mqtt_pass = std::getenv("SIM_MQTT_PASSWORD") ? std::getenv("SIM_MQTT_PASSWORD") : "";
    std::string server_url = std::getenv("SIM_SERVER_URL") ? std::getenv("SIM_SERVER_URL") : "http://server:8080";
    std::string device_id = (argc > 1) ? argv[1] : "";

    if (device_id.empty()) {
        std::printf("No device id given -- activating a fresh simulated device against %s ...\n", server_url.c_str());
        device_id = ActivateDevice(server_url, "SIM:00:00:00:00:01");
        if (device_id.empty()) {
            std::fprintf(stderr, "Activation failed -- pass a device id as argv[1] to skip it, "
                                  "or check SIM_SERVER_URL (%s) is reachable.\n", server_url.c_str());
            return 1;
        }
        std::printf("Activated as device %s\n", device_id.c_str());
    }

    HostMqtt mqtt(mqtt_host, mqtt_port, mqtt_user, mqtt_pass, device_id);
    mqtt.Subscribe("voice/control/down", [](const std::string& payload) {
        Log("< control/down: " + payload);
        if (IsTtsStopMessage(payload)) {
            SetEyes(EyesState::kIdle);
        }
    });
    mqtt.Subscribe("voice/audio/down", [](const std::string& payload) {
        Log("< audio/down: " + std::to_string(payload.size()) + " bytes (not decoded -- no speaker here)");
    });

    if (!mqtt.Connect()) {
        std::fprintf(stderr, "MQTT connect to %s:%d failed.\n", mqtt_host.c_str(), mqtt_port);
        return 1;
    }
    mqtt.StartLoop();

    initscr();
    cbreak();
    noecho();
    curs_set(1);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    WINDOW* status_win = newwin(rows - 3, cols, 0, 0);
    WINDOW* input_win = newwin(3, cols, rows - 3, 0);
    wtimeout(input_win, 100);  // non-blocking-ish: redraw status between keystrokes
    keypad(input_win, TRUE);

    Log("Device id: " + device_id);
    Log("Commands: click (toggle listen), hold (unlink/unpair), quit");

    std::string input_buffer;
    bool listening = false;
    bool running = true;

    while (running) {
        // --- redraw status pane ---
        werase(status_win);
        box(status_win, 0, 0);
        bool connected;
        EyesState eyes;
        std::deque<std::string> lines_copy;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            eyes = g_eyes_state;
            lines_copy = g_log_lines;
        }
        connected = mqtt.IsConnected();
        mvwprintw(status_win, 1, 2, "MQTT: %s   Device: %s", connected ? "connected" : "disconnected",
                  device_id.c_str());
        mvwprintw(status_win, 2, 2, "Eyes: %s", EyesArt(eyes));
        mvwhline(status_win, 3, 1, ACS_HLINE, cols - 2);
        int max_log_rows = (rows - 3) - 5;
        int start = static_cast<int>(lines_copy.size()) > max_log_rows
                        ? static_cast<int>(lines_copy.size()) - max_log_rows
                        : 0;
        int row = 4;
        for (int i = start; i < static_cast<int>(lines_copy.size()); i++) {
            mvwprintw(status_win, row++, 2, "%.*s", cols - 4, lines_copy[i].c_str());
        }
        wrefresh(status_win);

        // --- redraw input line ---
        werase(input_win);
        box(input_win, 0, 0);
        mvwprintw(input_win, 1, 2, "> %s", input_buffer.c_str());
        wrefresh(input_win);

        int ch = wgetch(input_win);
        if (ch == ERR) {
            continue;  // timeout -- just loop back to redraw
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            std::string command = input_buffer;
            input_buffer.clear();

            if (command == "click") {
                if (!listening) {
                    listening = true;
                    mqtt.Publish("voice/control/up", BuildHelloMessage(16000, 60));
                    mqtt.Publish("voice/control/up", BuildListenMessage(/*start=*/true));
                    SetEyes(EyesState::kListening);
                    Log("> click: started listening");
                } else {
                    listening = false;
                    mqtt.Publish("voice/control/up", BuildListenMessage(/*start=*/false));
                    SetEyes(EyesState::kTalking);
                    Log("> click: stopped listening, awaiting reply");
                }
            } else if (command == "hold") {
                mqtt.Publish("unlink", "{}");
                Log("> hold: published unlink (simulated BOOT-button 10s hold)");
            } else if (command == "quit" || command == "exit") {
                running = false;
            } else if (!command.empty()) {
                Log("> unknown command: " + command);
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!input_buffer.empty()) {
                input_buffer.pop_back();
            }
        } else if (ch >= 32 && ch < 127) {
            input_buffer.push_back(static_cast<char>(ch));
        }
    }

    delwin(status_win);
    delwin(input_win);
    endwin();
    mqtt.Stop();
    return 0;
}
