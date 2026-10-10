#pragma once
#include "Message.h"
#include <HistoricalClip.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>

struct RecentCaptureLive {
    struct Snapshot {
        bool Ready = false;
        bool Finished = false;
        bool UseDvr = false;
        std::string Error;
        std::vector<Witness::Camera::HistoricalClipSource> Files;
        uint64_t ClosedBytes = 0;
    };
    std::string Directory;
    std::shared_ptr<std::atomic<bool>> Cancelled;
    std::shared_ptr<std::atomic<double>> StopAt;
    mutable std::mutex Mutex;
    Snapshot Status;
    ~RecentCaptureLive() {
        try {
        if (!Cancelled || !Cancelled->load() || Directory.empty()) return;
        std::error_code error;
        const auto directory = std::filesystem::absolute(Directory, error).lexically_normal();
        if (error) return;
        for (const auto& file : Status.Files) {
            const auto path = std::filesystem::absolute(file.FilePath, error).lexically_normal();
            if (!error && path.parent_path() == directory) std::filesystem::remove(path, error);
            error.clear();
        }
        std::filesystem::remove(directory, error); // Never recursively delete.
        } catch (...) { /* Cleanup must never terminate a camera worker. */ }
    }
    Snapshot Get() const {std::lock_guard lock(Mutex); return Status;}
};
struct RecentCaptureLiveMessage : Message {
    explicit RecentCaptureLiveMessage(std::shared_ptr<RecentCaptureLive> live) : Live(std::move(live)) {}
    std::shared_ptr<RecentCaptureLive> Live;
};
struct RecentCaptureLiveRegistry {
    std::mutex Mutex;
    std::map<std::string, std::shared_ptr<RecentCaptureLive>> Requests;
    std::shared_ptr<RecentCaptureLive> Find(const std::string& path) {
        std::lock_guard lock(Mutex);
        const auto found = Requests.find(path); return found == Requests.end() ? nullptr : found->second;
    }
    void Add(const std::string& path, const std::shared_ptr<RecentCaptureLive>& value) {
        std::lock_guard lock(Mutex);
        for (auto it = Requests.begin(); it != Requests.end();) {
            // Jobs already exported retain their own final media. Retire source
            // bookkeeping only when the caller no longer needs the spool.
            if (it->second->Cancelled->load()) it = Requests.erase(it);
            else ++it;
        }
        Requests[path] = value;
    }
    void Remove(const std::string& path) {std::lock_guard lock(Mutex); Requests.erase(path);}
};
