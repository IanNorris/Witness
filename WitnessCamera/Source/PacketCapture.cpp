#include "PacketCapture.h"

#include <fstream>
#include <utility>

namespace Witness::Camera
{
PacketCapture::PacketCapture(const std::filesystem::path& directory, int durationSeconds)
	: _Directory(directory)
	, _Deadline(std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds))
{
	std::error_code error;
	_Ready = std::filesystem::create_directories(_Directory, error) && !error;
	if (_Ready)
	{
		std::ofstream manifest(_Directory / "events.jsonl", std::ios::binary | std::ios::trunc);
		std::ofstream input(_Directory / "input.bin", std::ios::binary | std::ios::trunc);
		std::ofstream output(_Directory / "output.bin", std::ios::binary | std::ios::trunc);
		_Ready = !!manifest && !!input && !!output;
	}
	if (_Ready)
		_Writer = std::thread(&PacketCapture::WriteLoop, this);
}

PacketCapture::~PacketCapture()
{
	{
		std::lock_guard lock(_Mutex);
		_Stopping = true;
	}
	_Wake.notify_one();
	if (_Writer.joinable())
		_Writer.join();
}

void PacketCapture::AddMetadata(std::string json)
{
	Enqueue({ std::move(json), {}, false, false });
}

void PacketCapture::AddInput(std::string json, const uint8_t* data, size_t size)
{
	if (!data || !size) return;
	Item item{ std::move(json), {}, true, true };
	item.Bytes.assign(data, data + size);
	Enqueue(std::move(item));
}

void PacketCapture::AddOutput(std::string json, const uint8_t* data, size_t size)
{
	if (!data || !size) return;
	Item item{ std::move(json), {}, false, true };
	item.Bytes.assign(data, data + size);
	Enqueue(std::move(item));
}

bool PacketCapture::FirstPacketForStream(int streamIndex)
{
	std::lock_guard lock(_Mutex);
	return _SeenStreams.insert(streamIndex).second;
}

void PacketCapture::Enqueue(Item&& item)
{
	if (!_Ready || _Complete.load()) return;
	std::lock_guard lock(_Mutex);
	if (_Stopping || std::chrono::steady_clock::now() >= _Deadline ||
		_AcceptedBytes + item.Bytes.size() > MaxCaptureBytes)
	{
		_Stopping = true;
		_Complete = true;
		_Wake.notify_one();
		return;
	}
	if (_QueuedBytes + item.Bytes.size() > MaxQueuedBytes)
	{
		++_RejectedRecords;
		_Stopping = true;
		_Complete = true;
		_Wake.notify_one();
		return;
	}
	_QueuedBytes += item.Bytes.size();
	_AcceptedBytes += item.Bytes.size();
	_Queue.push_back(std::move(item));
	_Wake.notify_one();
}

void PacketCapture::WriteLoop()
{
	std::ofstream manifest(_Directory / "events.jsonl", std::ios::binary | std::ios::trunc);
	std::ofstream input(_Directory / "input.bin", std::ios::binary | std::ios::trunc);
	std::ofstream output(_Directory / "output.bin", std::ios::binary | std::ios::trunc);
	if (!manifest || !input || !output)
	{
		_Complete = true;
		return;
	}
	uint64_t inputOffset = 0;
	uint64_t outputOffset = 0;
	for (;;)
	{
		Item item;
		{
			std::unique_lock lock(_Mutex);
			_Wake.wait_for(lock, std::chrono::milliseconds(250), [this]
			{
				return _Stopping || !_Queue.empty();
			});
			if (_Queue.empty() && std::chrono::steady_clock::now() >= _Deadline)
				_Stopping = true;
			if (_Queue.empty() && _Stopping) break;
			if (_Queue.empty()) continue;
			item = std::move(_Queue.front());
			_QueuedBytes -= item.Bytes.size();
			_Queue.pop_front();
		}
		if (item.Binary)
		{
			auto& file = item.Input ? input : output;
			auto& offset = item.Input ? inputOffset : outputOffset;
			file.write(reinterpret_cast<const char*>(item.Bytes.data()),
				static_cast<std::streamsize>(item.Bytes.size()));
			manifest << item.Json << ",\"offset\":" << offset
				<< ",\"bytes\":" << item.Bytes.size() << "}\n";
			offset += item.Bytes.size();
		}
		else manifest << item.Json << "}\n";
		if (!manifest || !input || !output)
		{
			_Complete = true;
			break;
		}
	}
	manifest << "{\"type\":\"summary\",\"inputBytes\":" << inputOffset
		<< ",\"outputBytes\":" << outputOffset
		<< ",\"queueRejectedRecords\":" << _RejectedRecords << "}\n";
	_Complete = true;
}
}
