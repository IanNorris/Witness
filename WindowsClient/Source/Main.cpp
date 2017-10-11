#include <windows.h>

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

using namespace Witness::Camera;

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	CreateDirectory( L"config", nullptr );

	auto Filter = std::make_shared<MotionFilter>();

	InputStream inputStream( lpCmdLine );
	OutputStream fileStream( "X:\\Output.mp4", &inputStream );

	inputStream.Initialize();
	fileStream.Initialize();

	while( inputStream.ProcessFrame( Filter.get(), &fileStream ) == CameraStreamError::Success );

	fileStream.CloseFile();

	return 0;
}