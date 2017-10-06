#include <windows.h>

#include <InputStream.h>

using namespace Witness::Camera;

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	InputStream rtspStream( lpCmdLine );

	rtspStream.Initialize();

	int i = 100;
	while( i-- )
	{
		rtspStream.ProcessFrame();
		//Sleep( 10 );
	}

	return 0;
}