#pragma once

#include "Stream.h"

namespace Witness{
namespace Camera{

class InputStream;

class CAMERA_API OutputStream : public Stream
{
public:
	OutputStream( const std::string& Path, const InputStream * InputStream = nullptr );
	virtual ~OutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( Stream* TargetStream ) override;
	virtual void Shutdown() override;

	void CloseFile();

private:

	const InputStream * m_InputStream;
};

}}
