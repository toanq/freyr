#include "test_console.h"

#include "synora_board.h"
#include "synora_display.h"

#include <esp_console.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <arpa/inet.h>
#include <lwip/netdb.h>

#include <cstdio>

static const char* TAG = "TestConsole";
static SynoraBoard* g_board = nullptr;

static int CmdTelemetry(int argc, char** argv) {
    if (g_board == nullptr) {
        return 1;
    }
    g_board->RefreshSensors();
    g_board->PublishTelemetry();
    return 0;
}

static int CmdFontTest(int argc, char** argv) {
    SynoraDisplay::GetInstance().ToggleFontTest();
    return 0;
}

// Diagnostic for the "device can't resolve synora.carcise.dev over MQTT but
// a phone on the same network resolves it fine" report -- calls the exact
// same getaddrinfo() esp-tls uses (esp_tls_hostname_to_fd() in esp-tls.c)
// directly from the console, and prints what DNS servers esp_netif actually
// has configured for the WiFi station interface, so a live device can be
// checked without waiting out esp-mqtt's own reconnect/retry cycle.
static int CmdDns(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "synora.carcise.dev";

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        printf("No WIFI_STA_DEF netif -- station not up\n");
    } else {
        for (int type = 0; type < 3; type++) {
            esp_netif_dns_info_t dns_info;
            if (esp_netif_get_dns_info(netif, static_cast<esp_netif_dns_type_t>(type), &dns_info) == ESP_OK &&
                dns_info.ip.u_addr.ip4.addr != 0) {
                printf("DNS[%d] = " IPSTR "\n", type, IP2STR(&dns_info.ip.u_addr.ip4));
            }
        }
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    int err = getaddrinfo(host, nullptr, &hints, &result);
    printf("getaddrinfo(%s) = %d\n", host, err);
    for (auto* p = result; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* addr = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            printf("  -> %s\n", inet_ntoa(addr->sin_addr));
        }
    }
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    return 0;
}

void InitializeTestConsole(SynoraBoard* board) {
    g_board = board;

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "synora>";

    // Native USB-Serial-JTAG (/dev/ttyACM0), not UART0 -- UART0/CONFIG_ESP_
    // CONSOLE_UART was the first thing tried, but produced zero bytes in
    // either direction on this board's ST-Link VCP (/dev/ttyACM1), including
    // plain ESP_LOG mirroring, so its physical wiring/routing on this custom
    // board was never actually confirmed reachable. USB-Serial-JTAG is the
    // one port already known-good all session (flashing, log capture), and
    // sdkconfig.defaults makes it the sole (primary, no secondary) console
    // to match -- see its comment for why running both would double-install
    // the same underlying driver.
    esp_console_dev_usb_serial_jtag_config_t usb_jtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_jtag_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t telemetry_cmd = {
        .command = "telemetry",
        .help = "Read sensors now and publish an MQTT telemetry/keepalive message",
        .hint = nullptr,
        .func = &CmdTelemetry,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&telemetry_cmd));

    const esp_console_cmd_t font_test_cmd = {
        .command = "fonttest",
        .help = "Toggle a screen dumping every Vietnamese vowel/tone glyph, to check font rendering",
        .hint = nullptr,
        .func = &CmdFontTest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&font_test_cmd));

    const esp_console_cmd_t dns_cmd = {
        .command = "dns",
        .help = "Show configured DNS servers and resolve a hostname (default synora.carcise.dev)",
        .hint = "[hostname]",
        .func = &CmdDns,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&dns_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Test console ready on /dev/ttyACM0 -- type 'help' to list commands");
}
