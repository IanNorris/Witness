#include "BaichuanCommandReplies.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using Witness::Camera::BaichuanCommandReplies;
using namespace std::chrono_literals;

static void Require(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}

int main() {
    try {
        BaichuanCommandReplies Replies;
        for (unsigned Status : {200u, 201u, 300u, 0u, 400u, 401u}) {
            auto Ticket = Replies.Begin(18, 0x12345601);
            Require(bool(Ticket), "register");
            Require(Replies.Complete(18, 0x12345601, Status, true), "complete before wait");
            Require(!Replies.Complete(18, 0x12345601, 200, true), "duplicate cannot override");
            Require(Replies.Wait(Ticket, 0ms) == (Status == 200 || Status == 201 || Status == 300), "status interpretation");
        }
        auto Ticket = Replies.Begin(18, 0x12345601);
        Require(!Replies.Begin(18, 0x12345601), "duplicate registration");
        Require(!Replies.Complete(19, 0x12345601, 200, true), "command mismatch");
        for (unsigned Identity : {0x12345602u, 0x12345701u, 0x12355601u})
            Require(!Replies.Complete(18, Identity, 200, true), "full identity mismatch");
        Require(!Replies.Complete(18, 0x12345601, 200, false), "legacy reply rejected");
        Require(!Replies.Wait(Ticket, 1ms), "timeout fails closed");
        Require(!Replies.Complete(18, 0x12345601, 200, true), "late reply ignored");
        auto Replacement = Replies.Begin(18, 0x12345601);
        Replies.Cancel(Ticket);
        Require(Replies.Complete(18, 0x12345601, 200, true), "old cancellation preserves replacement");
        Require(Replies.Wait(Replacement, 0ms), "replacement success");

        for (bool CancelAll : {false, true}) {
            Ticket = Replies.Begin(19, 42);
            std::promise<void> Started;
            auto Waiting = std::async(std::launch::async, [&] {
                Started.set_value();
                return Replies.Wait(Ticket, 5s);
            });
            Started.get_future().wait();
            if (CancelAll) Replies.CancelAll(); else Replies.Cancel(Ticket);
            Require(Waiting.wait_for(500ms) == std::future_status::ready, "cancellation wakes waiter");
            Require(!Waiting.get(), "cancelled request fails");
        }
        std::vector<BaichuanCommandReplies::Ticket> Tickets;
        for (unsigned Index = 0; Index < 32; ++Index) {
            Tickets.push_back(Replies.Begin(18, Index));
            Require(bool(Tickets.back()), "bounded registration");
        }
        Require(!Replies.Begin(18, 32), "bound enforced");
        Replies.CancelAll();
        for (const auto& Value : Tickets) Require(!Replies.Wait(Value, 0ms), "cancel all results");
        Require(bool(Replies.Begin(19, 32)), "capacity restored");
        Replies.CancelAll();

        auto First = Replies.Begin(18, 1);
        auto Second = Replies.Begin(19, 1);
        auto Waiting = std::async(std::launch::async, [&] { return Replies.Wait(First, 1s); });
        Require(Replies.Complete(19, 1, 400, true), "independent rejection");
        Require(Replies.Complete(18, 1, 200, true), "independent success");
        Require(!Replies.Wait(Second, 0ms) && Waiting.get(), "independent results");
        Require(!Replies.Wait({}, 0ms), "empty ticket fails");
        std::cout << "PASS Baichuan statuses, identity, timeout, cancellation, bounds and concurrent replies\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << Error.what() << '\n'; return 1;
    }
}
