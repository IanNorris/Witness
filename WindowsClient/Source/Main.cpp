#include <windows.h>

#include <Stream.h>

using namespace Witness::Camera;

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd )
{
	Stream S(lpCmdLine);

	return 0;
}