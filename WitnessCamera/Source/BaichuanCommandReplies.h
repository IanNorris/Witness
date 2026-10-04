#pragma once
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace Witness::Camera {
// Private bounded request/reply state. Socket writes are not acknowledgements.
class BaichuanCommandReplies {
    using Key = std::pair<unsigned, unsigned>;
    struct Request { Key Identity; bool Done = false, Success = false; };
    std::mutex Mutex;
    std::condition_variable Changed;
    std::map<Key, std::shared_ptr<Request>> Pending;
public:
    using Ticket = std::shared_ptr<Request>;
    bool IsPending(const Ticket& Value) {
        std::lock_guard Lock(Mutex);
        return Value && !Value->Done;
    }
    Ticket Begin(unsigned Command, unsigned Message) {
        std::lock_guard Lock(Mutex);
        const Key Identity{Command, Message};
        if (Pending.size() >= 32 || Pending.contains(Identity)) return {};
        auto Value = std::make_shared<Request>(); Value->Identity = Identity;
        Pending.emplace(Identity, Value); return Value;
    }
    bool Complete(unsigned Command, unsigned Message, unsigned Status, bool ModernHeader) {
        std::lock_guard Lock(Mutex);
        const auto Found = Pending.find({Command, Message});
        if (!ModernHeader || Found == Pending.end() || Found->second->Done) return false;
        Found->second->Done = true;
        Found->second->Success = Status == 200 || Status == 201 || Status == 300;
        Changed.notify_all(); return true;
    }
    bool Wait(const Ticket& Value, std::chrono::milliseconds Timeout) {
        if (!Value) return false;
        std::unique_lock Lock(Mutex);
        Changed.wait_for(Lock, Timeout, [&] { return Value->Done; });
        Value->Done = true;
        const auto Found = Pending.find(Value->Identity);
        if (Found != Pending.end() && Found->second == Value) Pending.erase(Found);
        return Value->Success;
    }
    void Cancel(const Ticket& Value) {
        if (!Value) return;
        std::lock_guard Lock(Mutex);
        Value->Done = true; Value->Success = false;
        const auto Found = Pending.find(Value->Identity);
        if (Found != Pending.end() && Found->second == Value) Pending.erase(Found);
        Changed.notify_all();
    }
    void CancelAll() {
        std::lock_guard Lock(Mutex);
        for (const auto& [Identity, Value] : Pending) { Value->Done = true; Value->Success = false; }
        Pending.clear(); Changed.notify_all();
    }
};
}
