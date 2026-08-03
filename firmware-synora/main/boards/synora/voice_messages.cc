#include "voice_messages.h"

#include <cJSON.h>

std::string BuildHelloMessage(int sample_rate_hz, int frame_duration_ms) {
    cJSON* hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddNumberToObject(hello, "version", 2);
    cJSON_AddStringToObject(hello, "transport", "mqtt");
    cJSON* features = cJSON_AddObjectToObject(hello, "features");
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON* audio_params = cJSON_AddObjectToObject(hello, "audio_params");
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", sample_rate_hz);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", frame_duration_ms);

    char* json = cJSON_PrintUnformatted(hello);
    std::string result(json);
    cJSON_free(json);
    cJSON_Delete(hello);
    return result;
}

std::string BuildListenMessage(bool start) {
    return start ? R"({"type":"listen","state":"start","mode":"manual"})"
                 : R"({"type":"listen","state":"stop"})";
}

bool IsTtsStopMessage(const std::string& payload) {
    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        return false;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    cJSON* state = cJSON_GetObjectItem(root, "state");
    bool is_tts_stop = cJSON_IsString(type) && std::string(type->valuestring) == "tts" &&
                        cJSON_IsString(state) && std::string(state->valuestring) == "stop";
    cJSON_Delete(root);
    return is_tts_stop;
}

bool IsMcpMessage(const std::string& payload) {
    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        return false;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    bool is_mcp = cJSON_IsString(type) && std::string(type->valuestring) == "mcp";
    cJSON_Delete(root);
    return is_mcp;
}
