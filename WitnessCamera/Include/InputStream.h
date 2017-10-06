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
	virtual CameraStreamError ProcessFrame() override;
	virtual void Shutdown() override;

private:

	
};

}}
