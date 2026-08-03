#include "test_util.h"

#include "../main/boards/synora/voice_messages.h"

#include <cJSON.h>

int RunVoiceMessagesTests() {
    TEST_SUITE_BEGIN();

    // BuildHelloMessage produces valid JSON with the fields/values the
    // server's hello parser expects (matches Synora.Protocol.ClientMessages.ParseHello).
    {
        std::string hello = BuildHelloMessage(16000, 60);
        cJSON* root = cJSON_Parse(hello.c_str());
        TEST_CHECK(root != nullptr);
        if (root != nullptr) {
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(root, "type")->valuestring), std::string("hello"));
            TEST_CHECK_EQ(cJSON_GetObjectItem(root, "version")->valueint, 2);
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(root, "transport")->valuestring), std::string("mqtt"));
            cJSON* features = cJSON_GetObjectItem(root, "features");
            TEST_CHECK(features != nullptr);
            if (features != nullptr) {
                TEST_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(features, "mcp")));
            }
            cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
            TEST_CHECK(audio_params != nullptr);
            if (audio_params != nullptr) {
                TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(audio_params, "format")->valuestring), std::string("opus"));
                TEST_CHECK_EQ(cJSON_GetObjectItem(audio_params, "sample_rate")->valueint, 16000);
                TEST_CHECK_EQ(cJSON_GetObjectItem(audio_params, "channels")->valueint, 1);
                TEST_CHECK_EQ(cJSON_GetObjectItem(audio_params, "frame_duration")->valueint, 60);
            }
            cJSON_Delete(root);
        }
    }

    // BuildListenMessage's two fixed shapes.
    {
        cJSON* start = cJSON_Parse(BuildListenMessage(true).c_str());
        TEST_CHECK(start != nullptr);
        if (start != nullptr) {
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(start, "type")->valuestring), std::string("listen"));
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(start, "state")->valuestring), std::string("start"));
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(start, "mode")->valuestring), std::string("manual"));
            cJSON_Delete(start);
        }

        cJSON* stop = cJSON_Parse(BuildListenMessage(false).c_str());
        TEST_CHECK(stop != nullptr);
        if (stop != nullptr) {
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(stop, "type")->valuestring), std::string("listen"));
            TEST_CHECK_EQ(std::string(cJSON_GetObjectItem(stop, "state")->valuestring), std::string("stop"));
            TEST_CHECK(cJSON_GetObjectItem(stop, "mode") == nullptr);
            cJSON_Delete(stop);
        }
    }

    // IsTtsStopMessage -- the one signal that flips the eyes back to idle.
    {
        TEST_CHECK(IsTtsStopMessage(R"({"type":"tts","state":"stop"})"));
        TEST_CHECK(!IsTtsStopMessage(R"({"type":"tts","state":"start"})"));
        TEST_CHECK(!IsTtsStopMessage(R"({"type":"tts","state":"sentence_start","text":"hi"})"));
        TEST_CHECK(!IsTtsStopMessage(R"({"type":"stt","text":"[loopback]"})"));
        TEST_CHECK(!IsTtsStopMessage(R"({"type":"llm","emotion":"happy"})"));
        TEST_CHECK(!IsTtsStopMessage("not json at all"));
        TEST_CHECK(!IsTtsStopMessage(""));
    }

    // IsMcpMessage -- the signal that routes a control message to McpServer.
    {
        TEST_CHECK(IsMcpMessage(R"({"type":"mcp","payload":{"jsonrpc":"2.0","method":"tools/call","id":1}})"));
        TEST_CHECK(!IsMcpMessage(R"({"type":"tts","state":"stop"})"));
        TEST_CHECK(!IsMcpMessage(R"({"type":"listen","state":"start"})"));
        TEST_CHECK(!IsMcpMessage("not json at all"));
        TEST_CHECK(!IsMcpMessage(""));
    }

    TEST_SUITE_END();
}
