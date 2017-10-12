#include "Listener.h"

#include <windows.h>

int wmain( int argc, wchar_t* argv[] )
{
	WitnessListener Listener( argv[1] );
	
	Listener.Start();

	while( true )
	{
		Sleep(100);
	}

	Listener.Stop();

	return 0;
}
