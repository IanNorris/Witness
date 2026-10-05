#pragma once
#include "ClientHealthHistory.h"
#include <crow/json.h>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace Witness::ClientHealth {
inline bool BoundedDocument(std::string_view Body) {
    if (Body.size() > 96 * 1024) return false;
    int Depth = 0;
    bool Quoted = false, Escaped = false;
    for (char C : Body) {
        if (Quoted) {
            if (Escaped) Escaped = false;
            else if (C == '\\') Escaped = true;
            else if (C == '"') Quoted = false;
        } else if (C == '"') Quoted = true;
        else if (C == '{' || C == '[') { if (++Depth > 16) return false; }
        else if (C == '}' || C == ']') { if (--Depth < 0) return false; }
    }
    return !Quoted && Depth == 0;
}
inline std::string String(const crow::json::rvalue& Object, const char* Field, size_t Limit) {
    if (!Object.has(Field) || Object[Field].t() != crow::json::type::String) return {};
    std::string Value = Object[Field].s();
    if (Value.size() > Limit) Value.resize(Limit);
    // Do not persist URLs/credentials accidentally carried by error strings.
    if (Value.find("://") != std::string::npos || Value.find('@') != std::string::npos) return "[redacted]";
    return Value;
}
inline void Numbers(const crow::json::rvalue& Source, crow::json::wvalue& Target,
                    std::initializer_list<const char*> Fields) {
    for (const auto* Field : Fields) if (Source.has(Field) && Source[Field].t() == crow::json::type::Number) {
        const double Value = Source[Field].d();
        if (std::isfinite(Value) && std::abs(Value) <= 1e15) Target[Field] = Value;
    }
}
inline void Texts(const crow::json::rvalue& Source, crow::json::wvalue& Target,
                  std::initializer_list<const char*> Fields, size_t Limit = 80) {
    for (const auto* Field : Fields) { auto Value = String(Source, Field, Limit); if (!Value.empty()) Target[Field] = std::move(Value); }
}
inline void Booleans(const crow::json::rvalue& Source, crow::json::wvalue& Target,
                     std::initializer_list<const char*> Fields) {
    for (const auto* Field : Fields) if (Source.has(Field)) {
        const auto Type = Source[Field].t();
        if (Type == crow::json::type::True || Type == crow::json::type::False) Target[Field] = Type == crow::json::type::True;
    }
}
inline crow::json::wvalue Drift(const crow::json::rvalue& Source) {
    crow::json::wvalue Result;
    Texts(Source, Result, {"t", "visibility", "sourceBufferMode"}, 40);
    Numbers(Source, Result, {"currentTime", "bufferedEnd", "lagMs", "playbackRate", "readyState", "generation",
        "sampleIntervalMs", "playbackAdvanceMs", "bufferAdvanceMs", "lagChangeMs", "fragmentAgeMs", "appendAgeMs",
        "appendQueueLength", "appendQueueBytes", "appendQueueOldestAgeMs", "appendOperationAgeMs", "safeKeyframes",
        "futureSafeKeyframes", "highLatencyMs", "schedulingDelayMs", "maxFragmentGapMs", "fragmentsSinceSample",
        "maxAppendDurationMs", "lastAppendDurationMs", "lastBufferAdvanceMs", "lastPartialDurationMs", "segmentIndex",
        "partIndex", "timestampOffsetSeconds", "volume", "videoFrames", "droppedVideoFrames",
        "bufferedRangeCount", "nextBufferedStartSeconds", "playheadGapMs", "headroomInCurrentRangeMs"});
    Booleans(Source, Result, {"paused", "seeking", "catchUpActive", "hasAudio", "sourceBufferUpdating", "partialIndependent", "partialSeekSafe", "muted", "playheadBuffered"});
    return Result;
}
inline Sample Sanitize(const crow::json::rvalue& Source, int UserUID, std::string Username) {
    if (Source.t() != crow::json::type::Object || !Source.has("players") ||
        Source["players"].t() != crow::json::type::List || Source["players"].size() > 64)
        throw std::invalid_argument("invalid client health snapshot");
    Sample Value;
    Value.UserUID = UserUID; Value.Username = std::move(Username);
    Value.BrowserID = String(Source, "browserId", 80); Value.SessionID = String(Source, "sessionId", 80);
    crow::json::wvalue Result;
    Result["browserId"] = Value.BrowserID; Result["sessionId"] = Value.SessionID;
    Texts(Source, Result, {"sampledAtUtc", "buildHash", "visibilityState"}, 80);
    Texts(Source, Result, {"userAgent"}, 256);
    auto Path = String(Source, "path", 128); Path.resize(Path.find_first_of("?#") == std::string::npos ? Path.size() : Path.find_first_of("?#"));
    Result["path"] = Path;
    Booleans(Source, Result, {"playersTruncated"});
    std::vector<crow::json::wvalue> Players;
    for (const auto& Player : Source["players"]) {
        if (Player.t() != crow::json::type::Object || String(Player, "id", 80).empty()) throw std::invalid_argument("invalid player");
        crow::json::wvalue Out;
        Texts(Player, Out, {"id", "selectedStream", "mediaSourceState", "sourceBufferOperation", "lastEventType", "lastRestartReason"});
        Numbers(Player, Out, {"latencyMs", "readyState", "wsReadyState", "wsOpenAgeMs", "reconnectPendingMs", "lastFragAge",
            "appendQueueLength", "appendQueueBytes", "appendQueueOldestAgeMs", "sourceBufferGeneration", "sourceBufferOperationAgeMs",
            "pipelineAgeMs", "lastAppendAgeMs", "totalVideoFrames", "droppedVideoFrames", "corruptedVideoFrames", "restartCount",
            "stallCount", "errorCount", "totalFragments", "integrityMismatchCount", "decodeCorruptionCount", "renderSuppressionCount"});
        Booleans(Player, Out, {"sourceBufferUpdating", "waitingForKeyframe", "hasInitialBuffer", "awaitingInit"});
        // Preserve the causal snapshot after the rolling sample/event lists move on,
        // using the same bounded scalar allowlist rather than arbitrary event JSON.
        if (Player.has("lastDriftRestart") && Player["lastDriftRestart"].t() == crow::json::type::Object) {
            const auto& Restart = Player["lastDriftRestart"];
            crow::json::wvalue Event;
            Texts(Restart, Event, {"t", "type", "reason"});
            Numbers(Restart, Event, {"lagMs", "generation"});
            if (Restart.has("drift") && Restart["drift"].t() == crow::json::type::Object)
                Event["drift"] = Drift(Restart["drift"]);
            Out["lastDriftRestart"] = std::move(Event);
        }
        if (Player.has("lastError") && Player["lastError"].t() == crow::json::type::Object) {
            const auto& Error = Player["lastError"];
            crow::json::wvalue Event;
            Texts(Error, Event, {"t", "type", "name", "reason", "mediaSourceState"});
            Numbers(Error, Event, {"code", "generation", "consecutive", "readyState", "wsReadyState"});
            Booleans(Error, Event, {"intentionalClose"});
            if (Error.has("drift") && Error["drift"].t() == crow::json::type::Object)
                Event["drift"] = Drift(Error["drift"]);
            Out["lastError"] = std::move(Event);
        }
        if (Player.has("driftSamples") && Player["driftSamples"].t() == crow::json::type::List) {
            std::vector<crow::json::wvalue> Samples;
            const auto& List = Player["driftSamples"];
            for (size_t Index = List.size() > 4 ? List.size() - 4 : 0; Index < List.size(); ++Index)
                if (List[Index].t() == crow::json::type::Object) Samples.push_back(Drift(List[Index]));
            Out["driftSamples"] = std::move(Samples);
        }
        if (Player.has("recentEvents") && Player["recentEvents"].t() == crow::json::type::List) {
            std::vector<crow::json::wvalue> Events;
            const auto& List = Player["recentEvents"];
            for (size_t Index = List.size() > 8 ? List.size() - 8 : 0; Index < List.size(); ++Index) {
                if (List[Index].t() != crow::json::type::Object) continue;
                crow::json::wvalue Event;
                Texts(List[Index], Event, {"t", "type", "reason", "name", "mediaSourceState", "mimeType"});
                Numbers(List[Index], Event, {"code", "readyState", "generation", "segmentIndex", "partIndex", "backoffMs", "consecutive", "lagMs"});
                // Omit free-form messages and nested arbitrary event payloads.
                Events.push_back(std::move(Event));
            }
            Out["recentEvents"] = std::move(Events);
        }
        Players.push_back(std::move(Out));
    }
    Result["players"] = std::move(Players);
    Value.Json = Result.dump();
    if (Value.Json.size() > History::MaxPayloadBytes) throw std::length_error("client health snapshot too large");
    return Value;
}
}
