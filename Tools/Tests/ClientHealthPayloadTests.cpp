#include "ClientHealthPayload.h"
#include <iostream>
#include <stdexcept>
using namespace Witness::ClientHealth;
static void Check(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
int main() {
    try {
        Check(!BoundedDocument(std::string(20, '[') + std::string(20, ']')), "deep JSON accepted");
        Check(BoundedDocument(R"({"message":"braces [ { inside a quoted string"})"), "quoted delimiters counted as structure");
        Check(!BoundedDocument("{\"unterminated"), "unterminated document accepted");
        auto Body = crow::json::load(R"({"browserId":"browser-1","sessionId":"tab-1","username":"forged","userUid":999,
          "csrf":"secret","url":"https://secret@camera","path":"/dashboard?token=secret","players":[{"id":"8_mse",
          "latencyMs":1200,"password":"secret","recentEvents":[{"type":"error","message":"https://secret@camera",
          "reason":"https://secret@camera","code":3}],"driftSamples":[{"t":"2026-10-04T00:00:00Z","lagMs":1200,"url":"secret"}]}]})");
        auto Value = Sanitize(Body, 7, "server-user");
        Check(Value.UserUID == 7 && Value.Username == "server-user", "client spoofed owner");
        Check(Value.Json.find("secret") == std::string::npos && Value.Json.find("forged") == std::string::npos, "non-allowlisted credentials persisted");
        auto Clean = crow::json::load(Value.Json);
        Check(Clean["players"][0]["latencyMs"].d() == 1200, "numeric evidence lost");
        Check(std::string(Clean["path"].s()) == "/dashboard", "query stored");
        auto Recovery = crow::json::load(R"({"players":[{"id":"8_mse","lastDriftRestart":{
          "t":"2026-10-05T03:36:40Z","type":"driftRestart","reason":"drift","lagMs":148000,
          "message":"https://secret@camera","driftSamples":[{"password":"secret"}],
          "drift":{"visibility":"visible","paused":true,"currentTime":73,"bufferedEnd":222,
            "lagMs":149000,"fragmentAgeMs":76,"appendAgeMs":75,"generation":1,
            "buffered":[[193,222]],"password":"secret"}}}]})");
        auto RecoveryValue = Sanitize(Recovery, 7, "server-user");
        auto RecoveryClean = crow::json::load(RecoveryValue.Json);
        const auto& Restart = RecoveryClean["players"][0]["lastDriftRestart"];
        Check(Restart["lagMs"].d() == 148000 && std::string(Restart["reason"].s()) == "drift", "recovery summary lost");
        Check(Restart["drift"]["fragmentAgeMs"].d() == 76 && Restart["drift"]["paused"].b(), "causal scalar snapshot lost");
        Check(!Restart.has("driftSamples") && !Restart["drift"].has("buffered"), "unbounded recovery details persisted");
        Check(RecoveryValue.Json.find("secret") == std::string::npos, "recovery credentials persisted");
        auto MalformedRecovery = crow::json::load(R"({"players":[{"id":"8_mse","lastDriftRestart":"wrong","lastError":["wrong"]}]})");
        auto MalformedClean = crow::json::load(Sanitize(MalformedRecovery, 7, "server-user").Json);
        Check(!MalformedClean["players"][0].has("lastDriftRestart"), "malformed recovery accepted");
        Check(!MalformedClean["players"][0].has("lastError"), "malformed last error accepted");
        auto ErrorBody = crow::json::load(R"({"players":[{"id":"8_mse","lastError":{
          "t":"2026-10-05T03:00:00Z","type":"wsError","wsReadyState":3,"generation":4,
          "intentionalClose":true,"message":"https://secret@camera","password":"secret",
          "drift":{"paused":true,"lagMs":99000,"buffered":[[1,2]],"url":"secret"}}}]})");
        auto ErrorValue = Sanitize(ErrorBody, 7, "server-user");
        auto ErrorClean = crow::json::load(ErrorValue.Json);
        const auto& ErrorSnapshot = ErrorClean["players"][0]["lastError"];
        Check(ErrorSnapshot["wsReadyState"].d() == 3 && ErrorSnapshot["generation"].d() == 4 &&
              ErrorSnapshot["intentionalClose"].b(), "error transport context lost");
        Check(ErrorSnapshot["drift"]["lagMs"].d() == 99000, "error scalar evidence lost");
        Check(ErrorValue.Json.find("secret") == std::string::npos && !ErrorSnapshot["drift"].has("buffered"), "error secrets or arrays persisted");
        auto Bad = crow::json::load(R"({"players":"wrong"})");
        bool Rejected = false;
        try { Sanitize(Bad, 7, "server-user"); } catch (const std::invalid_argument&) { Rejected = true; }
        Check(Rejected, "invalid players accepted");
        std::cout << "Client health allowlist and owner validation passed\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
