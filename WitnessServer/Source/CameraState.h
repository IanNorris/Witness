#pragma once

class CameraWorker;

struct CameraState
{
	CameraState()
	: Status("Starting")
	, LastLargePreviewTimestamp(0)
	, LastSmallPreviewTimestamp(0)
	, IsRecording(false)
	, IsManualRecording(false)
	{

	}

	std::shared_ptr<CameraWorker> Worker;
	std::string Name;

	std::vector<unsigned char> PreviewThumbnail;
	std::vector< int > TriggeredActions;

	std::unordered_map< uint64_t, std::vector<unsigned char> > ClipThumbnails;

	std::string Status;

	uint64_t LastLargePreviewTimestamp;
	uint64_t LastSmallPreviewTimestamp;

	bool IsRecording;
	bool IsManualRecording;
	bool WantLargePreview;
};