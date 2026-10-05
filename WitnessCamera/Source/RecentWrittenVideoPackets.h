#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Witness::Camera {
// Functional duplicate evidence, independent of optional diagnostic storage.
// Match the historical lookup exactly: last 32 written video packets, provided
// they are still within the last 1024 recorded packet dispositions (audio and
// rejected packets also consume that horizon). Hash collisions retain the old
// conservative duplicate classification.
class RecentWrittenVideoPackets {
public:
    static constexpr std::size_t PacketHorizon = 1024;
    static constexpr std::size_t VideoHorizon = 32;

    void Record(bool WrittenVideo, std::uint64_t PayloadHash) noexcept {
        ++_Sequence;
        if (!WrittenVideo) return;
        _Entries[_Position] = {PayloadHash, _Sequence};
        _Position = (_Position + 1) % VideoHorizon;
        if (_Count < VideoHorizon) ++_Count;
    }

    bool Contains(std::uint64_t PayloadHash) const noexcept {
        for (std::size_t Offset = 0; Offset < _Count; ++Offset) {
            const auto& Entry = _Entries[(_Position + VideoHorizon - 1 - Offset) % VideoHorizon];
            // Unsigned subtraction also handles sequence wrap without changing
            // the bounded relative age of an entry.
            if (_Sequence - Entry.Sequence >= PacketHorizon) break;
            if (Entry.Hash == PayloadHash) return true;
        }
        return false;
    }

private:
    struct Entry { std::uint64_t Hash = 0, Sequence = 0; };
    std::array<Entry, VideoHorizon> _Entries{};
    std::uint64_t _Sequence = 0;
    std::size_t _Position = 0, _Count = 0;
};
}
