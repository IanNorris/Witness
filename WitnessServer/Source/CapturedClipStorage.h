#pragma once
#include <HistoricalClip.h>
struct sqlite3;
// Adds media metadata without changing existing clip rows or their unique index.
void EnsureCapturedClipSchema(sqlite3* database);
int64_t RegisterCapturedClip(const std::string& databaseFilename, int camera, const std::string& path,
    const Witness::Camera::HistoricalClipResult& media, double requestedFrom, double requestedTo);
