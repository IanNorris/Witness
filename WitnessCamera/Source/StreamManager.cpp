#include "OutputStream.h"
#include "StreamManager.h"
#include "DebugBind.h"

namespace Witness{
namespace Camera{

CAMERA_API DebugConsole* TargetDebugConsole = nullptr;

StreamManager::StreamManager()
: m_DiagnosticStream( nullptr )
{
}

StreamManager::~StreamManager()
{
	CloseDiagnosticStream();
}

OutputStream* StreamManager::GetDiagnosticStream( int Width, int Height )
{
	if( !m_DiagnosticStream )
	{
		//m_DiagnosticStream = std::make_shared<OutputStream>( "X:\\Diagnostics.mp4", Width, Height, 25, true );
	}

	return m_DiagnosticStream.get();
}

void StreamManager::CloseDiagnosticStream()
{
	if( m_DiagnosticStream )
	{
		m_DiagnosticStream->CloseFile();
		m_DiagnosticStream.reset();
	}
}

void StreamManager::WriteFrame( unsigned int Width, unsigned int Height, void* Data, ClassificationResult* Result )
{

}

}}
