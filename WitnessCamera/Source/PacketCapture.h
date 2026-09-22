#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Witness::Camera
{
// Opt-in, bounded diagnostic capture. The camera thread only copies into a
// small queue; file I/O is performed on the writer thread.
class PacketCapture
{
public:
	PacketCapture(const std::filesystem::path& directory, int durationSeconds);
	~PacketCapture();
	PacketCapture(const PacketCapture&) = delete;
	PacketCapture& operator=(const PacketCapture&) = delete;

	bool Ready() const { return _Ready; }
	bool Complete() const
	{
		return _Complete.load() || std::chrono::steady_clock::now() >= _Deadline;
	}
	const std::filesystem::path& Directory() const { return _Directory; }
	void AddMetadata(std::string json);
	void AddInput(std::string json, const uint8_t* data, size_t size);
	void AddOutput(std::string json, const uint8_t* data, size_t size);
	bool FirstPacketForStream(int streamIndex);

private:
	struct Item
	{
		std::string Json;
		std::vector<uint8_t> Bytes;
		bool Input = false;
		bool Binary = false;
	};
	void Enqueue(Item&& item);
	void WriteLoop();

	static constexpr size_t MaxCaptureBytes = 128ULL * 1024 * 1024;
	static constexpr size_t MaxQueuedBytes = 8ULL * 1024 * 1024;
	std::filesystem::path _Directory;
	std::chrono::steady_clock::time_point _Deadline;
	std::mutex _Mutex;
	std::condition_variable _Wake;
	std::deque<Item> _Queue;
	std::set<int> _SeenStreams;
	std::thread _Writer;
	size_t _QueuedBytes = 0;
	size_t _AcceptedBytes = 0;
	std::atomic<uint64_t> _RejectedRecords{ 0 };
	std::atomic<bool> _Complete{ false };
	bool _Stopping = false;
	bool _Ready = false;
};
}
