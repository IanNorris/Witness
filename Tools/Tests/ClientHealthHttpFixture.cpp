// Loopback-only integration fixture: real Crow cookies/auth and history worker,
// temporary databases, no camera workers and no authentication bypass.
#include "CrowAuth.h"
#include "ClientHealthEndpoint.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

static std::string ProductionQuery(const std::string& Source, const char* Name) {
    const std::string Prefix = std::string("std::string ") + Name + " = R\"RAW(";
    const auto Begin = Source.find(Prefix);
    if (Begin == std::string::npos) throw std::runtime_error("production query not found");
    const auto End = Source.find(")RAW\";", Begin + Prefix.size());
    if (End == std::string::npos) throw std::runtime_error("production query terminator not found");
    return Source.substr(Begin + Prefix.size(), End - Begin - Prefix.size());
}

int main(int Argc, char** Argv) {
    if (Argc != 3) return 2;
    const std::filesystem::path Root = std::filesystem::absolute(Argv[1]);
    if (Root.filename().string().find("witness-health-http-") != 0 ||
        !std::filesystem::is_regular_file(Root / "auth.db")) return 2;
    try {
        GlobalContext Context;
        Context.Database = std::make_shared<SQLiteDatabase>((Root / "auth.db").string(), "", false,
            [](const std::string&) { throw std::runtime_error("fixture auth database error"); });
        const auto Queries = ReadFileToString(Argv[2]);
        for (const auto* Name : {"VerifySession", "VerifySessionAndCSRF", "FindUserForAuth"})
            Context.Database->CreateQuery(Name, ProductionQuery(Queries, Name));
        Context.ClientHealthHistory = std::make_shared<Witness::ClientHealth::History>((Root / "history.db").string());
        const auto ReadyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!Context.ClientHealthHistory->GetSnapshot().Ready) {
            if (std::chrono::steady_clock::now() >= ReadyDeadline) return 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!Context.ClientHealthHistory->GetSnapshot().StorageAvailable) return 3;
        crow::SimpleApp App;
        std::atomic<bool> Stop{false};
        std::atomic<bool> Ready{false};
        CROW_ROUTE(App, "/debug/client-health").methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)
        ([&](const crow::request& Req, crow::response& Res) {
            if (!Ready.load(std::memory_order_acquire)) { Res.code = 503; Res.end(); return; }
            Witness::ClientHealth::Serve(Req, Res, Context.ClientHealthHistory,
                [&](const crow::json::rvalue* Body, bool Write) {
                    return CrowAuth::IsAuthenticated(Context, Req, Body,
                        Write ? CrowAuth::Action::ReadWrite : CrowAuth::Action::Read,
                        Write ? CrowAuth::Privilege::Normal : CrowAuth::Privilege::Administrator);
                }, [&](int User) -> std::optional<std::string> {
                    std::optional<std::string> Username;
                    SQLiteDatabaseQueryInstance Query(Context.Database, "FindUserForAuth");
                    Query->Bind("@UserUID", User);
                    Query->Execute([&](const SQLiteDatabaseQuery& Row) {
                        Username = Row.GetColumnValueText(1); return true;
                    });
                    return Username;
                });
        });
        CROW_ROUTE(App, "/fixture/stop").methods(crow::HTTPMethod::Post)
        ([&] { Stop = true; return crow::response(200); });
        App.bindaddr("127.0.0.1").port(0).concurrency(2).signal_clear().loglevel(crow::LogLevel::Critical);
        auto Running = App.run_async();
        if (App.wait_for_server_start(std::chrono::seconds(5)) == std::cv_status::timeout) {
            App.stop(); Running.get(); return 4;
        }
        Context.Port = App.port();
        Ready.store(true, std::memory_order_release);
        std::cout << "fixturePort=" << Context.Port << std::endl;
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!Stop && std::chrono::steady_clock::now() < Deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Allow the stop response to complete before shutting down the listener.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        App.stop(); Running.get();
        Context.ClientHealthHistory.reset(); // joins/flushed worker before exit
        return Stop ? 0 : 5;
    } catch (const std::exception&) { std::cerr << "health HTTP fixture failed\n"; return 1; }
}
