#pragma once

struct CameraState
{
	CameraState()
	: Status(_T("Starting"))
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
	bool IsRecording;
	bool IsManualRecording;
};