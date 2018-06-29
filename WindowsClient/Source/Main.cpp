#include <windows.h>

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>
#include <ImageProcessingJob.h>

using namespace Witness::Camera;

uint64_t GetFrameIndex()
{
	static uint64_t Value = 0;
	return Value++;
}

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	CreateDirectory( L"config", nullptr );

	auto Filter = std::make_shared<MotionFilter>( 0.1 );

	ImageProcessingJobQueue Queue;

	InputStreamSetup Setup;
	Setup.GetTimestamp = GetFrameIndex;

	InputStream inputStream( Setup, 0, &Queue, lpCmdLine );
	OutputStream fileStream( "X:\\Output.mp4", &inputStream );

	inputStream.Initialize();
	fileStream.Initialize();

	while( inputStream.ProcessFrame( std::static_pointer_cast<IRecordFilter>(Filter), &fileStream ) == CameraStreamError::Success );

	fileStream.CloseFile();

	return 0;
}