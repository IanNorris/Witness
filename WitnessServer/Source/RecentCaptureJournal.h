#pragma once
#include "RecentCaptureJobs.h"
#include <memory>

class RecentCaptureJournal {
public:
    explicit RecentCaptureJournal(const std::string& filename);
    ~RecentCaptureJournal();
    std::vector<RecentCaptureJobs::Snapshot> Load();
    void Save(const std::vector<RecentCaptureJobs::Snapshot>& values);
private:
    struct Impl;
    std::unique_ptr<Impl> Data;
};
