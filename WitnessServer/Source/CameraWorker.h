#pragma once

#include "WorkerBase.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

class ObservingMotionFilter;

using namespace Witness::Camera;

class CameraWorker : public WorkerBase
{
public:
	CameraWorker(const int CameraID, const string_t& InputPath, const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	, Path( InputPath )
	, CameraID( CameraID )
	, IsConnected( false )
	{}

private:

	virtual void WorkerInit() override;
	virtual void WorkerShutdown() override;
	virtual void WorkerMain() override;

	void OnClipFinished();

	shared_ptr<OutputStream> RecordStream;

	shared_ptr<InputStream> CameraStream;
	shared_ptr<ObservingMotionFilter> Filter;

	string_t Path;
	int CameraID;
	bool IsConnected;
};
