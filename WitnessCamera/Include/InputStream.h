#pragma once

#include "Stream.h"

namespace Witness{
namespace Camera{



class CAMERA_API InputStream : public Stream
{
public:
	InputStream( const std::string& StreamURL, int StreamIndex = 0 );
	virtual ~InputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( IRecordFilter* Filter, Stream* TargetStream ) override;
	virtual void Shutdown() override;

private:

	const static int64_t ConnectionTimeout = 5;

	static int InterruptCallback( void* Opaque );

	StreamManager* m_StreamManager;

	int64_t TimeStarted;
	bool IsConnecting;
};

}}
