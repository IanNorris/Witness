#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct OperationalCameraStatus
{
	int Id = 0;
	std::string Name;
	std::string State;
	bool Recording = false;
	bool MotionActive = false;
	bool MainAvailable = false;
	bool MainEstablished = false;
	bool PreviewConnected = false;
	std::string Codec;
	int Width = 0;
	int Height = 0;
	int RetainedSegments = 0;
	int Reconnects = 0;
	uint64_t DroppedPackets = 0;
	uint64_t RepairedTimestamps = 0;
	uint64_t CorruptPackets = 0;
	uint64_t PendingEssential = 0;
	uint64_t PendingAI = 0;
	double OldestEssentialMs = 0.0;
	double OldestAIMs = 0.0;
};

struct OperationalStatus
{
	std::string BuildHash;
	uint16_t Port = 0;
	double HostCpuPercent = -1.0;
	uint64_t HostTotalMemoryBytes = 0;
	uint64_t HostAvailableMemoryBytes = 0;
	uint64_t ProcessWorkingSetBytes = 0;
	uint64_t ProcessPrivateBytes = 0;
	std::vector<OperationalCameraStatus> Cameras;
};
