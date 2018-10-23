#pragma once

class CameraWorker;

struct CameraState
{
	CameraState()
	: Status(_T("Starting"))
	, LastLargePreviewTimestamp(0)
	, LastSmallPreviewTimestamp(0)
	, IsRecording(false)
	, IsManualRecording(false)
	{

	}

	shared_ptr<CameraWorker> Worker;
	string_t Name;

	vector<unsigned char> PreviewThumbnail;
	vector< int > TriggeredActions;

	unordered_map< uint64_t, vector<unsigned char> > ClipThumbnails;

	string_t Status;

	uint64_t LastLargePreviewTimestamp;
	uint64_t LastSmallPreviewTimestamp;

	bool IsRecording;
	bool IsManualRecording;
	bool WantLargePreview;
};