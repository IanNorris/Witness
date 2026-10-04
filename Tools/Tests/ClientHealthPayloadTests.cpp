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
        auto Bad = crow::json::load(R"({"players":"wrong"})");
        bool Rejected = false;
        try { Sanitize(Bad, 7, "server-user"); } catch (const std::invalid_argument&) { Rejected = true; }
        Check(Rejected, "invalid players accepted");
        std::cout << "Client health allowlist and owner validation passed\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
