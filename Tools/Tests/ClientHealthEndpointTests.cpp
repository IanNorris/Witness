#include "ClientHealthEndpoint.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace Witness::ClientHealth;
static void Check(bool OK, const char* Message) { if (!OK) throw std::runtime_error(Message); }
int main(int Argc, char** Argv) {
    if (Argc != 2 || std::filesystem::exists(Argv[1])) return 2;
    try {
        {
            auto Store = std::make_shared<History>(Argv[1]);
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!Store->GetSnapshot().Ready) { Check(std::chrono::steady_clock::now() < Deadline, "store startup timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            bool LoggedIn = false, Admin = false;
            auto Auth = [&](const crow::json::rvalue* Body, bool Write) {
                if (!LoggedIn || (!Write && !Admin)) return -1;
                if (Write && (!Body || std::string((*Body)["csrf"].s()) != "fixture-token")) return -1;
                return 7;
            };
            auto Lookup = [](int UID) -> std::optional<std::string> { Check(UID == 7, "client controlled user identity"); return "server-user"; };
            auto Request = [&](crow::HTTPMethod Method, std::string Body = "") {
                crow::request Req; Req.method = Method; Req.body = std::move(Body);
                crow::response Res; Serve(Req, Res, Store, Auth, Lookup); return Res;
            };
            const std::string Payload = R"({"csrf":"fixture-token","snapshot":{"browserId":"browser","sessionId":"tab","userUid":999,"username":"forged","players":[]}})";
            Check(Request(crow::HTTPMethod::Get).code == 403, "unauthenticated history read allowed");
            Check(Request(crow::HTTPMethod::Post, Payload).code == 403, "unauthenticated submission allowed");
            LoggedIn = true;
            Check(Request(crow::HTTPMethod::Get).code == 403, "non-admin history read allowed");
            Check(Request(crow::HTTPMethod::Post, R"({"csrf":"wrong","snapshot":{"players":[]}})").code == 403, "bad CSRF accepted");
            Check(Request(crow::HTTPMethod::Post, R"({"snapshot":{"players":[]}})").code == 400, "missing CSRF accepted");
            Check(Request(crow::HTTPMethod::Post, Payload).code == 202, "valid authenticated submission rejected");
            Check(Request(crow::HTTPMethod::Post, Payload).code == 429, "rate limit not exposed");
            while (Store->GetSnapshot().DurableThroughSequence == 0) {
                Check(std::chrono::steady_clock::now() < Deadline, "store write timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Admin = true;
            auto Response = Request(crow::HTTPMethod::Get);
            Check(Response.code == 200, "admin export rejected");
            auto Export = crow::json::load(Response.body);
            Check(Export && Export["history"].size() == 1, "historical export missing");
            Check(Export["history"][0]["userUid"].i() == 7 && std::string(Export["history"][0]["username"].s()) == "server-user", "spoofed ownership persisted");
            Check(Request(crow::HTTPMethod::Post, std::string(100 * 1024, ' ')).code == 413, "oversized body accepted");
            Check(Request(crow::HTTPMethod::Delete).code == 405, "unsupported method accepted");
        }
        std::filesystem::remove(Argv[1]); // This test created this exact fixture.
        std::cout << "Client health endpoint auth, CSRF, ownership and bounds passed\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
