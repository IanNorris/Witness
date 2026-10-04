#pragma once
#include "ClientHealthPayload.h"
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <optional>

namespace Witness::ClientHealth {
// The production adapter supplies CrowAuth, with ReadWrite/Normal for POST
// and Read/Administrator for GET. Keeping the boundary here permits isolated
// auth/CSRF tests without opening cameras or changing the live database.
template<class Authenticate, class LookupUser>
void Serve(const crow::request& req, crow::response& res, const std::shared_ptr<History>& Store,
           Authenticate Auth, LookupUser Lookup) {
    res.set_header("Content-Type", "application/json");
    res.set_header("Cache-Control", "no-store");
    if (req.method != crow::HTTPMethod::Get && req.method != crow::HTTPMethod::Post) { res.code = 405; res.end(); return; }
    const bool Write = req.method == crow::HTTPMethod::Post;
    if (req.body.size() > 96 * 1024) { res.code = 413; res.end(); return; }
    if (Write && !BoundedDocument(req.body)) { res.code = 400; res.end(); return; }
    auto Body = crow::json::load(Write ? req.body : "{}");
    if (Write && (!Body || Body.t() != crow::json::type::Object || !Body.has("csrf") ||
        Body["csrf"].t() != crow::json::type::String)) { res.code = 400; res.end(); return; }
    const int User = Auth(Write ? &Body : nullptr, Write);
    if (User < 0) { res.code = 403; res.end(); return; }
    if (!Store) { res.code = 503; res.end(); return; }
    if (!Write) { res.body = Store->ExportJson(); res.code = 200; res.end(); return; }
    try {
        if (!Body.has("snapshot")) throw std::invalid_argument("missing snapshot");
        const auto Username = Lookup(User);
        if (!Username) { res.code = 503; res.end(); return; }
        const auto Result = Store->Submit(Sanitize(Body["snapshot"], User, *Username));
        using ResultType = History::SubmitResult;
        res.code = Result == ResultType::Accepted ? 202 : Result == ResultType::Invalid ? 400 :
            Result == ResultType::RateLimited ? 429 : 503;
        if (res.code == 429 || res.code == 503) res.set_header("Retry-After", "15");
        res.body = res.code == 202 ? "{\"accepted\":true}" : "{\"accepted\":false}";
    } catch (const std::length_error&) { res.code = 413; }
      catch (const std::invalid_argument&) { res.code = 400; }
    res.end();
}
}
