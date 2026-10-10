#include "HistoricalClip.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace Witness::Camera;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        Check(argc == 3 || argc == 6, "Usage: HistoricalClipTests input new-output-directory [duration cut-from cut-to]");
        const fs::path input(argv[1]), output(argv[2]);
        Check(fs::is_regular_file(input) && !fs::exists(output), "Input missing or output directory already exists");
        fs::create_directories(output);
        if (argc == 6) {
            // Read-only real-media probe. Outputs belong in ignored artifacts,
            // never alongside the original recording or in tracked fixtures.
            const double duration = std::stod(argv[3]);
            auto real = RemuxHistoricalClip({{input.string(), 1000, 1000 + duration, 0}},
                1000 + std::stod(argv[4]), 1000 + std::stod(argv[5]), (output / "cut.mp4").string());
            Check(real.Success, real.Error.c_str());
            std::cout << "PASS media remux video=" << real.VideoPackets << " audio=" << real.AudioPackets
                << " from=" << real.ActualFrom << " to=" << real.ActualTo << " gaps=" << real.Gaps.size() << '\n';
            return 0;
        }
        const std::vector<HistoricalClipSource> sources{{input.string(), 1000, 1008, 0}};
        auto cut = RemuxHistoricalClip(sources, 1003, 1006, (output / "cut.mp4").string());
        if (!cut.Success) std::cerr << cut.Error << '\n';
        Check(cut.Success && cut.VideoPackets > 0 && cut.AudioPackets > 0, "Audio/video capture failed");
        Check(cut.ActualFrom <= 1003 && cut.ActualFrom >= 1000 && cut.ActualTo >= 1006
            && cut.ActualTo < 1006.5 && cut.Gaps.empty(), "Incorrect capture coverage");
        const auto bytes = fs::file_size(output / "cut.mp4");
        Check(!RemuxHistoricalClip(sources, 1003, 1006, (output / "cut.mp4").string()).Success
            && fs::file_size(output / "cut.mp4") == bytes, "Existing output was overwritten");
        Check(!RemuxHistoricalClip(sources, 1003, 1006, (output / "cancel.mp4").string(), [] {return true;}).Success
            && !fs::exists(output / "cancel.mp4"), "Cancelled export published a file");
        auto gap = RemuxHistoricalClip(sources, 999, 1009, (output / "gap.mp4").string());
        Check(gap.Success && gap.Gaps.size() == 2 && gap.Gaps.front().From == 999
            && gap.Gaps.back().To == 1009, "Unavailable boundaries were not reported");
        Check(!RemuxHistoricalClip(sources, 1006, 1003, (output / "invalid.mp4").string()).Success,
            "Invalid range was accepted");
        auto joined = RemuxHistoricalClip({{input.string(), 1000, 1008, 0},
            {input.string(), 1010, 1018, 0}}, 1003, 1017, (output / "joined.mp4").string());
        Check(joined.Success && joined.Coverage.size() == 2 && joined.Gaps.size() == 1
            && joined.Gaps.front().From >= 1007.9 && joined.Gaps.front().To == 1010,
            "Multiple sources did not preserve the real gap");
        auto adjacent = RemuxHistoricalClip({{input.string(), 1000, 1008, 0},
            {input.string(), 1008, 1016, 0}}, 1003, 1015, (output / "adjacent.mp4").string());
        Check(adjacent.Success && adjacent.Coverage.size() == 2 && adjacent.Gaps.empty(),
            "Adjacent source boundary failed or introduced a false gap");
        auto snapshot = RemuxHistoricalClip({{input.string(), 1000, 1008,
            static_cast<int64_t>(fs::file_size(input))}}, 1003, 1006, (output / "snapshot.mp4").string());
        Check(snapshot.Success, "Committed source snapshot export failed");
        Check(!RemuxHistoricalClip({{input.string(), 1000, 1008,
            static_cast<int64_t>(fs::file_size(input)) + 1}}, 1003, 1006,
            (output / "incomplete.mp4").string()).Success, "Incomplete committed prefix was accepted");
        for (const auto& file : fs::directory_iterator(output))
            Check(file.path().extension() == ".mp4", "Temporary export file leaked");
        std::cout << "PASS historical remux, coverage, cancellation and no overwrite\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
