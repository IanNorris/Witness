#include <windows.h>

#include <InputStream.h>
#include <OutputStream.h>

using namespace Witness::Camera;

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	InputStream rtspStream( lpCmdLine );
	OutputStream fileStream( "X:\\Output.mp4", &rtspStream );

	rtspStream.Initialize();
	fileStream.Initialize();

	int i = 500;
	while( i-- )
	{
		rtspStream.ProcessFrame( &fileStream );
	}

	fileStream.CloseFile();

	return 0;
}