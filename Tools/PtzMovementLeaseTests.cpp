#include "../WitnessServer/Source/PtzMovementLease.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
using namespace std::chrono_literals;
static void Check(bool condition) { if (!condition) throw std::runtime_error("PTZ lease assertion failed"); }
struct Signal {
    std::mutex Mutex; std::condition_variable Ready; unsigned Count = 0;
    void Set() { std::lock_guard<std::mutex> lock(Mutex); ++Count; Ready.notify_all(); }
    bool Wait(unsigned count = 1) {
        std::unique_lock<std::mutex> lock(Mutex);
        return Ready.wait_for(lock, 2s, [&] { return Count >= count; });
    }
};
int main()
{
    try {
        { // No browser Stop: expiry still stops the motor.
            Signal stopped;
            PtzMovementLease lease({}, 20ms, 10ms);
            Check(lease.Move(10, [] { return true; }, [&] { stopped.Set(); return true; }));
            Check(stopped.Wait());
        }
        { // Retained Stop does not need a replacement transport or new lane.
            int stopped = 0;
            PtzMovementLease lease({}, 10s, 10ms);
            for (int id = 1; id <= 100; ++id) Check(!lease.StopExisting(id).has_value());
            Check(lease.Move(10, [] { return true; }, [&] { ++stopped; return true; }));
            const auto result = lease.StopExisting(10);
            Check(result.has_value() && *result && stopped == 1);
            Check(!lease.StopExisting(10).has_value());
            Check(stopped == 1);
        }
        { // Failed retained Stop stays armed; caller must not try a new transport.
            Signal retried;
            std::atomic<int> attempts{0};
            PtzMovementLease lease({}, 10s, 20ms);
            Check(lease.Move(10, [] { return true; }, [&] {
                if (++attempts == 1) return false;
                retried.Set(); return true;
            }));
            const auto result = lease.StopExisting(10);
            Check(result.has_value() && !*result);
            Check(retried.Wait());
            Check(attempts == 2);
            Check(!lease.StopExisting(10).has_value());
        }
        { // Ambiguous move failure + failed Stop: retry without user involvement.
            Signal stopped; std::atomic<int> attempts{0};
            PtzMovementLease lease({}, 20ms, 10ms);
            Check(!lease.Move(10, []() -> bool { throw std::runtime_error("lost ack"); }, [&] {
                if (++attempts < 3) return false;
                stopped.Set(); return true;
            }));
            Check(stopped.Wait()); Check(attempts == 3);
        }
        { // Linked callers share ID: old Stop precedes replacement movement.
            std::string order;
            PtzMovementLease lease({}, 10s, 10ms);
            Check(lease.Move(10, [&] { order += 'L'; return true; }, [&] { order += 'S'; return true; }));
            Check(lease.Move(10, [&] { order += 'R'; return true; }, [&] { order += 'T'; return true; }));
            Check(lease.Stop(10, [] { return false; }));
            Check(order == "LSRT");
            Check(lease.Position(10, [&] { order += 'P'; return true; }));
            Check(order == "LSRTP");
        }
        { // A slow camera cannot hold another camera's safety stop hostage.
            Signal entered, secondStopped; std::promise<void> release;
            auto ready = release.get_future().share();
            PtzMovementLease lease({}, 20ms, 10ms);
            Check(lease.Move(10, [] { return true; }, [&] { entered.Set(); ready.wait(); return true; }));
            const bool enteredStop = entered.Wait();
            if (!enteredStop) release.set_value();
            Check(enteredStop);
            Check(lease.Move(12, [] { return true; }, [&] { secondStopped.Set(); return true; }));
            const bool independent = secondStopped.Wait();
            release.set_value(); // release even if the assertion below fails
            Check(independent);
        }
        { // Shutdown makes a final Stop attempt before destroying transports.
            int stopped = 0;
            { PtzMovementLease lease({}, 10s, 10ms);
              Check(lease.Move(10, [] { return true; }, [&] { ++stopped; return true; })); }
            Check(stopped == 1);
        }
        { // A preset cannot supersede a movement whose Stop is failing.
            std::atomic<bool> allow{false}; std::atomic<int> preset{0};
            PtzMovementLease lease({}, 10s, 10ms);
            Check(lease.Move(10, [] { return true; }, [&] { return allow.load(); }));
            Check(!lease.Position(10, [&] { ++preset; return true; }));
            Check(preset == 0); allow = true;
            Check(lease.Position(10, [&] { ++preset; return true; })); Check(preset == 1);
        }
        std::cout << "PASS: expiry, ambiguous delivery, retry, linked ordering, independent lanes, shutdown and presets\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
