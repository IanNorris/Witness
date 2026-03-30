#pragma once

// ---------------------------------------------------------------------------
// Lightweight service/daemon status notification shim.
//
// On Windows this forwards to SetServiceStatus (SCM).
// On Linux this writes to $NOTIFY_SOCKET (systemd sd_notify protocol)
// without requiring a libsystemd dependency.
//
// Both implementations are header-inline so no extra source file is needed.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#include <windows.h>

namespace Witness
{
// Callers hold a pointer to a shared ServiceNotifier that wraps the
// SERVICE_STATUS_HANDLE obtained after RegisterServiceCtrlHandler.
struct ServiceNotifier
{
SERVICE_STATUS        Status  = {};
SERVICE_STATUS_HANDLE Handle  = nullptr;
bool                  IsService = true;

void Update( DWORD state, DWORD waitHintSecs = 0, DWORD errorCode = NO_ERROR )
{
if( !IsService || !Handle ) return;
Status.dwCurrentState  = state;
Status.dwWin32ExitCode = errorCode;
Status.dwWaitHint      = waitHintSecs * 1000;
++Status.dwCheckPoint;
SetServiceStatus( Handle, &Status );
}
};
} // namespace Witness

#else // Linux / POSIX

#include <cstring>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Witness
{
// On Linux the "notifier" just writes to NOTIFY_SOCKET if present.
struct ServiceNotifier
{
bool IsService = false; // set to true when running under systemd

void Update( /* state is unused on Linux */ int /*state*/,
             int /*waitHintSecs*/ = 0, int /*errorCode*/ = 0 ) {}

void NotifyReady()
{
SdNotify( "READY=1\nSTATUS=Running" );
}

void NotifyStarting()
{
SdNotify( "STATUS=Starting..." );
}

void NotifyStopping()
{
SdNotify( "STOPPING=1\nSTATUS=Shutting down..." );
}

private:
static void SdNotify( const char* msg )
{
const char* sock = getenv( "NOTIFY_SOCKET" );
if( !sock || sock[0] == '\0' ) return;

int fd = socket( AF_UNIX, SOCK_DGRAM, 0 );
if( fd < 0 ) return;

struct sockaddr_un addr = {};
addr.sun_family = AF_UNIX;

bool abstract = ( sock[0] == '@' );
const char* path = abstract ? sock + 1 : sock;
strncpy( abstract ? addr.sun_path + 1 : addr.sun_path,
         path, sizeof( addr.sun_path ) - 2 );

socklen_t len = static_cast<socklen_t>(
offsetof( struct sockaddr_un, sun_path ) +
( abstract ? 1 : 0 ) + strlen( path ) );

sendto( fd, msg, strlen( msg ), MSG_NOSIGNAL,
        reinterpret_cast<const struct sockaddr*>( &addr ), len );
close( fd );
}
};
} // namespace Witness

#endif // _WIN32
