#pragma once

#include <memory>

namespace Witness{
namespace Camera{

struct ClassificationResult;
class OutputStream;

class StreamManager
{
public:

	StreamManager();
	~StreamManager();

	OutputStream* GetDiagnosticStream( int Width, int Height );
	void CloseDiagnosticStream();

	void WriteFrame( unsigned int Width, unsigned int Height, void* Data, ClassificationResult* Result );

private:

	std::shared_ptr<OutputStream> m_DiagnosticStream;
	std::shared_ptr<OutputStream> m_OutputStream;
};

}}
