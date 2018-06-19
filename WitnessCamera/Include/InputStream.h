#pragma once

#include "Stream.h"
#include "ImageProcessingJob.h"

namespace Witness{
namespace Camera{

class CAMERA_API InputStream : public Stream
{
public:
	InputStream( int SourceID, ImageProcessingJobQueue* JobQueue, const std::string& StreamURL, int StreamIndex = 0 );
	virtual ~InputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream ) override;
	virtual void Shutdown() override;

private:

	const static int64_t ConnectionTimeout = 5;

	static int InterruptCallback( void* Opaque );

	ImageProcessingJobQueue* CommonJobQueue;

	StreamManager* m_StreamManager;

	int UniqueSourceID;
	int64_t TimeStarted;
	bool IsConnecting;
};

}}
