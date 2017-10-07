#include <windows.h>

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

using namespace Witness::Camera;

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	CreateDirectory( L"config", nullptr );

	auto Filter = std::make_shared<MotionFilter>();

	InputStream rtspStream( lpCmdLine );
	OutputStream fileStream( "X:\\Output.mp4", &rtspStream );

	rtspStream.Initialize();
	fileStream.Initialize();

	while( rtspStream.ProcessFrame( Filter.get(), &fileStream ) == CameraStreamError::Success );

	fileStream.CloseFile();

	return 0;
}