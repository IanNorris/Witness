#include "CrowListener.h"
#include "Common.h"
#include "Database.h"
#include "SetupConfig.h"
#include "SetupServer.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"
#include "Platform/ServiceManager.h"
#include <Stream.h>
#include <ONNXDetectionFilter.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
#include <windows.h>
#include <minmax.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#else
#include <algorithm>
#include <csignal>
#include <strings.h>   // strcasecmp
using std::min;
using std::max;
#endif

#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <Log.h>

#ifdef CROW_ENABLE_SSL
#include <openssl/applink.c>
#endif

// ---------------------------------------------------------------------------
// Cross-platform helpers
// ---------------------------------------------------------------------------

static std::filesystem::path GetExeDir()
{
#ifdef _WIN32
wchar_t buf[MAX_PATH] = {};
GetModuleFileNameW( nullptr, buf, MAX_PATH );
return std::filesystem::path( buf ).parent_path();
#else
return std::filesystem::canonical( "/proc/self/exe" ).parent_path();
#endif
}

// Case-insensitive string compare (already using std::string args everywhere).
static bool ArgMatch( const std::string& a, const char* b )
{
#ifdef _WIN32
return _stricmp( a.c_str(), b ) == 0;
#else
return strcasecmp( a.c_str(), b ) == 0;
#endif
}

std::filesystem::path GetConfigFilePath( std::string Filename );

// ---------------------------------------------------------------------------
// Windows: SCM service install / uninstall helpers
// ---------------------------------------------------------------------------

#ifdef _WIN32

#define SERVICE_NAME         L"WitnessCameraServer"
#define SERVICE_DISPLAY_NAME L"Witness Camera Service - CCTV Monitoring Server"
#define SERVICE_START_TYPE   SERVICE_AUTO_START
#define SERVICE_ACCOUNT      L"NT AUTHORITY\\NetworkService"
#define SERVICE_PASSWORD     L""

static bool UpdateWindowsService( const wchar_t* exePath, bool install )
{
SC_HANDLE scm = OpenSCManagerW( nullptr, nullptr,
SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE );
if( !scm )
{
LOG_ERROR( "Unable to connect to service manager - admin access required." );
return false;
}

if( install )
{
// Remove stale entry first.
SC_HANDLE old = OpenServiceW( scm, SERVICE_NAME,
SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | DELETE );
if( old ) { CloseServiceHandle( old ); UpdateWindowsService( exePath, false ); }

SC_HANDLE svc = CreateServiceW( scm, SERVICE_NAME, SERVICE_DISPLAY_NAME,
SERVICE_QUERY_STATUS, SERVICE_WIN32_OWN_PROCESS, SERVICE_START_TYPE,
SERVICE_ERROR_NORMAL, exePath, nullptr, nullptr, nullptr,
SERVICE_ACCOUNT, SERVICE_PASSWORD );
if( !svc )
{
CloseServiceHandle( scm );
LOG_ERROR( "Unable to create new service." );
return false;
}
CloseServiceHandle( svc );
CloseServiceHandle( scm );
LOG_INFO( "Created service." );
return true;
}
else
{
SC_HANDLE svc = OpenServiceW( scm, SERVICE_NAME,
SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | DELETE );
if( !svc )
{
CloseServiceHandle( scm );
LOG_ERROR( "Service not found." );
return false;
}

LOG_INFO( "Stopping service." );
SERVICE_STATUS status = {};
if( ControlService( svc, SERVICE_CONTROL_STOP, &status ) )
{
for( int t = 60; t > 0 && status.dwCurrentState != SERVICE_STOPPED; --t )
{
std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
QueryServiceStatus( svc, &status );
}
}

if( status.dwCurrentState == SERVICE_STOPPED )
LOG_INFO( "Service stopped cleanly." );
else
LOG_WARNING( "Service failed to stop cleanly." );

if( !DeleteService( svc ) )
LOG_WARNING( "Could not delete service." );
else
LOG_INFO( "Deleted service." );

CloseServiceHandle( svc );
CloseServiceHandle( scm );
return true;
}
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// Shared server run body
// ---------------------------------------------------------------------------

static bool           ContinueRunning = true;
static WitnessServer* GlobalServer    = nullptr;
static int            ReturnValue     = 0;
static Witness::ServiceNotifier* g_Notifier = nullptr;

static void RequestShutdown()
{
ContinueRunning = false;
if( GlobalServer )
GlobalServer->RequestShutdown();
}

static void RunServer()
{
#ifdef _WIN32
// Request high-resolution timer on Windows.
struct ScopedTimePeriod
{
UINT Period;
ScopedTimePeriod( UINT p ) : Period( p ) { timeBeginPeriod( p ); }
~ScopedTimePeriod() { timeEndPeriod( Period ); }
};
TIMECAPS tc;
timeGetDevCaps( &tc, sizeof( tc ) );
UINT res = min( max( tc.wPeriodMin, (UINT)0 ), tc.wPeriodMax );
ScopedTimePeriod timePeriod( res );
#endif

DebugConsole DebugConsoleInstance;
Witness::Camera::TargetDebugConsole = &DebugConsoleInstance;

WitnessServer Server;
GlobalServer = &Server;

if( sodium_init() == -1 )
{
LOG_ERROR( "Unable to initialize libsodium." );
if( g_Notifier ) g_Notifier->Update( 0, 0, 1 );
ReturnValue = 1;
return;
}

if( !Server.Initialize( &DebugConsoleInstance ) )
{
if( g_Notifier ) g_Notifier->Update( 0, 0, 1 );
ReturnValue = 1;
return;
}

#ifdef _WIN32
if( g_Notifier ) g_Notifier->Update( SERVICE_RUNNING );
#else
if( g_Notifier ) g_Notifier->NotifyReady();
#endif

Server.MessageLoop( ContinueRunning );

GlobalServer = nullptr;
Server.Shutdown();

Witness::Camera::TargetDebugConsole = nullptr;

#ifdef _WIN32
if( g_Notifier ) g_Notifier->Update( SERVICE_STOPPED );
#else
if( g_Notifier ) g_Notifier->NotifyStopping();
#endif

ReturnValue = 0;
}

// ---------------------------------------------------------------------------
// Windows: service control dispatcher
// ---------------------------------------------------------------------------

#ifdef _WIN32

static Witness::ServiceNotifier s_WinNotifier;

static void WINAPI ServiceController( DWORD action )
{
switch( action )
{
case SERVICE_CONTROL_STOP:
case SERVICE_CONTROL_SHUTDOWN:
s_WinNotifier.Update( SERVICE_STOP_PENDING, 20 );
RequestShutdown();
break;
case SERVICE_CONTROL_INTERROGATE:
break;
}
}

static BOOL WINAPI ConsoleHandlerRoutine( DWORD /*type*/ )
{
RequestShutdown();
return TRUE;
}

static void WINAPI ServiceMain( DWORD /*argc*/, PWSTR* /*argv*/ )
{
s_WinNotifier.Handle = RegisterServiceCtrlHandlerW( SERVICE_NAME, ServiceController );
s_WinNotifier.Status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
s_WinNotifier.Status.dwCurrentState     = SERVICE_START_PENDING;
s_WinNotifier.Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
s_WinNotifier.Status.dwWin32ExitCode    = NO_ERROR;
g_Notifier = &s_WinNotifier;

s_WinNotifier.Update( SERVICE_START_PENDING, 30 );
std::this_thread::sleep_for( std::chrono::seconds( 10 ) );

RunServer();
}

#else // Linux / POSIX

static void PosixSignalHandler( int /*sig*/ )
{
RequestShutdown();
}

#endif

// ---------------------------------------------------------------------------
// Shared CLI command dispatch (args already normalized to std::string)
// ---------------------------------------------------------------------------

static int HandleCliCommands( const std::vector<std::string>& args )
{
if( args.size() < 2 )
return -1; // no command — fall through to service start

#ifdef _WIN32
if( ArgMatch( args[1], "/installservice" ) )
{
wchar_t exePath[MAX_PATH] = {};
GetModuleFileNameW( nullptr, exePath, MAX_PATH );
return UpdateWindowsService( exePath, true ) ? 0 : 1;
}
else if( ArgMatch( args[1], "/uninstallservice" ) )
{
return UpdateWindowsService( nullptr, false ) ? 0 : 1;
}
else
#endif
if( ArgMatch( args[1], "/createdb" ) )
{
auto dbFile = GetConfigFilePath( "server.db" );
Database::InitializeDatabase( dbFile.string() );
return 0;
}
else if( ArgMatch( args[1], "/websetup" ) )
{
auto dbFile = GetConfigFilePath( "server.db" );
auto DB = Database::InitializeDatabase( dbFile.string() );

if( sodium_init() == -1 )
{
LOG_ERROR( "Unable to initialize libsodium." );
return 1;
}

std::string staticRoot = ( GetExeDir() / "Web" ).string();
SetupServer setup( DB, staticRoot );
setup.Run();
return 0;
}
else if( ArgMatch( args[1], "/setup" ) )
{
auto dbFile = GetConfigFilePath( "server.db" );
auto DB = Database::InitializeDatabase( dbFile.string() );

if( args.size() >= 4 && ArgMatch( args[2], "--json" ) )
{
SetupConfig config;
if( !config.LoadFromJson( args[3].c_str() ) ) return 1;
if( !config.ApplyToDatabase( DB ) )
{
LOG_ERROR( "Failed to apply configuration." );
return 1;
}
LOG_INFO( "Configuration applied successfully." );
return 0;
}
else
{
if( sodium_init() == -1 )
{
LOG_ERROR( "Unable to initialize libsodium." );
return 1;
}

SetupConfig config;

LOG_INFO( "" );
LOG_INFO( "========================================" );
LOG_INFO( "  Witness Interactive Setup" );
LOG_INFO( "========================================" );
LOG_INFO( "" );

std::cout << "Admin username: ";
std::getline( std::cin, config.Username );

std::cout << "Admin password: ";
SetStdinEcho( false );
std::getline( std::cin, config.Password );
SetStdinEcho( true );
std::cout << std::endl;

std::cout << "Server hostname:port (e.g. localhost:8080): ";
std::getline( std::cin, config.Hostname );

std::cout << "TLS mode [NoSecurity/SelfSigned/LetsEncrypt/Manual]: ";
std::getline( std::cin, config.TlsMode );
if( config.TlsMode.empty() ) config.TlsMode = "NoSecurity";

#ifdef _WIN32
const char* defaultCache = "C:\\WitnessCache";
#else
const char* defaultCache = "/var/cache/witness";
#endif
std::cout << "Cache path [" << defaultCache << "]: ";
std::getline( std::cin, config.CachePath );
if( config.CachePath.empty() ) config.CachePath = defaultCache;

if( !config.ApplyToDatabase( DB ) )
{
LOG_ERROR( "Failed to apply configuration." );
return 1;
}

LOG_INFO( "Setup complete. Start WitnessServer normally to run." );
return 0;
}
}
else if( ArgMatch( args[1], "/test-cuda" ) )
{
const char* cudnnPath = args.size() >= 3 ? args[2].c_str() : nullptr;
auto modelPath = GetExeDir() / "models" / "yolo26n.onnx";
std::string modelPathStr = modelPath.string();
bool ok = Witness::Camera::TestCudaAvailability(
std::filesystem::exists( modelPath ) ? modelPathStr.c_str() : nullptr,
cudnnPath );
return ok ? 0 : 1;
}
else if( ArgMatch( args[1], "/test-detection" ) )
{
if( args.size() < 3 )
{
std::cerr << "Usage: WitnessServer /test-detection <clip.mp4> "
             "[--clahe] [--model <path>] [--confidence <float>] "
             "[--save-clahe <dir>]" << std::endl;
return 1;
}

const std::string& clipPath = args[2];
bool useClahe = false;
float confidence = 0.3f;
std::string saveClaheDir;
std::string modelPath = ( GetExeDir() / "models" / "yolo26n.onnx" ).string();

for( int i = 3; i < (int)args.size(); i++ )
{
if( ArgMatch( args[i], "--clahe" ) )
useClahe = true;
else if( ArgMatch( args[i], "--model" ) && i + 1 < (int)args.size() )
modelPath = args[++i];
else if( ArgMatch( args[i], "--confidence" ) && i + 1 < (int)args.size() )
confidence = (float)atof( args[++i].c_str() );
else if( ArgMatch( args[i], "--save-clahe" ) && i + 1 < (int)args.size() )
saveClaheDir = args[++i];
}

if( !std::filesystem::exists( clipPath ) )
{
std::cerr << "Clip not found: " << clipPath << std::endl;
return 1;
}
if( !std::filesystem::exists( modelPath ) )
{
std::cerr << "Model not found: " << modelPath << std::endl;
return 1;
}

avformat_network_init();

Witness::Camera::MotionChainNode dummyChain;
Witness::Camera::ONNXDetectionFilter filter( dummyChain, modelPath.c_str(), confidence, false );

if( !filter.IsModelLoaded() )
{
std::cerr << "Failed to load model: " << modelPath << std::endl;
return 1;
}

AVFormatContext* fmtCtx = nullptr;
if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) < 0 )
{
std::cerr << "Failed to open clip: " << clipPath << std::endl;
return 1;
}
avformat_find_stream_info( fmtCtx, nullptr );

int videoIdx = -1;
for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
{
if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
{
videoIdx = (int)i;
break;
}
}
if( videoIdx < 0 )
{
std::cerr << "No video stream found" << std::endl;
avformat_close_input( &fmtCtx );
return 1;
}

auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
auto codec    = avcodec_find_decoder( codecpar->codec_id );
auto codecCtx = avcodec_alloc_context3( codec );
avcodec_parameters_to_context( codecCtx, codecpar );
avcodec_open2( codecCtx, codec, nullptr );

SwsContext* swsCtx = sws_getContext(
codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
SWS_BILINEAR, nullptr, nullptr, nullptr );

double durationSec = fmtCtx->duration > 0
? (double)fmtCtx->duration / AV_TIME_BASE : 0.0;

AVPacket* pkt      = av_packet_alloc();
AVFrame*  frame    = av_frame_alloc();
AVFrame*  bgrFrame = av_frame_alloc();

int w = codecCtx->width, h = codecCtx->height;
int bgrBufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, w, h, 1 );
std::vector<uint8_t> bgrBuf( bgrBufSize );
av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize,
bgrBuf.data(), AV_PIX_FMT_BGR24, w, h, 1 );

std::cout << "Clip: "     << clipPath  << "\n"
          << "Duration: " << durationSec << "s  Resolution: " << w << "x" << h << "\n"
          << "Model: "    << modelPath << "  Confidence: " << confidence << "\n"
          << "CLAHE: "    << (useClahe ? "ON" : "OFF") << "\n---\n";

if( !saveClaheDir.empty() )
std::filesystem::create_directories( saveClaheDir );

double sampleInterval = 2.0, currentTime = 0.0;
int    frameNum = 0;

while( currentTime <= durationSec || currentTime == 0.0 )
{
if( currentTime > 0.0 )
{
int64_t seek = (int64_t)( currentTime * AV_TIME_BASE );
av_seek_frame( fmtCtx, -1, seek, AVSEEK_FLAG_BACKWARD );
avcodec_flush_buffers( codecCtx );
}

bool gotFrame = false;
while( av_read_frame( fmtCtx, pkt ) >= 0 )
{
if( pkt->stream_index == videoIdx )
{
avcodec_send_packet( codecCtx, pkt );
if( avcodec_receive_frame( codecCtx, frame ) == 0 )
{
sws_scale( swsCtx, frame->data, frame->linesize,
0, h, bgrFrame->data, bgrFrame->linesize );

cv::Mat mat( h, w, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );
auto lighting = Witness::Camera::ClassifyLighting( mat );

const char* lightStr = "Unknown";
if( lighting == Witness::Camera::LightingCondition::Day )   lightStr = "Day";
else if( lighting == Witness::Camera::LightingCondition::Night ) lightStr = "Night";

cv::Mat detMat = mat;
if( useClahe && lighting == Witness::Camera::LightingCondition::Night )
detMat = Witness::Camera::ApplyCLAHE( mat );

if( !saveClaheDir.empty() &&
lighting == Witness::Camera::LightingCondition::Night )
{
auto clahe = Witness::Camera::ApplyCLAHE( mat );
auto orig  = ( std::filesystem::path( saveClaheDir ) /
( "frame_" + std::to_string( frameNum ) + "_original.jpg" ) ).string();
auto clp   = ( std::filesystem::path( saveClaheDir ) /
( "frame_" + std::to_string( frameNum ) + "_clahe.jpg" ) ).string();
cv::imwrite( orig, mat );
cv::imwrite( clp,  clahe );
std::cout << "  Saved: " << orig << " and " << clp << "\n";
}

auto detections = filter.DetectFrame( detMat );
std::cout << "Frame " << frameNum << " @ " << currentTime
          << "s  Lighting: " << lightStr;
if( detections.empty() )
{
std::cout << "  (no detections)\n";
}
else
{
std::cout << "  Detections:\n";
for( auto& d : detections )
{
char buf[128];
snprintf( buf, sizeof( buf ), "    %-12s  %.1f%%",
d.ClassName.c_str(), d.Confidence * 100.0f );
std::cout << buf << "\n";
}
}

gotFrame = true;
av_packet_unref( pkt );
break;
}
}
av_packet_unref( pkt );
}

if( !gotFrame ) break;
frameNum++;
currentTime += sampleInterval;
}

av_frame_free( &bgrFrame );
av_frame_free( &frame );
av_packet_free( &pkt );
sws_freeContext( swsCtx );
avcodec_free_context( &codecCtx );
avformat_close_input( &fmtCtx );
return 0;
}
else if( ArgMatch( args[1], "/apply-config" ) && args.size() >= 3 )
{
const std::string& configPath = args[2];
SetupConfig config;
if( !config.LoadFromJson( configPath.c_str() ) ) return 1;

auto dbFile = GetConfigFilePath( "server.db" );
auto DB = Database::InitializeDatabase( dbFile.string() );

if( !config.ApplyToDatabase( DB ) )
{
LOG_ERROR( "Failed to apply configuration." );
return 1;
}

bool success = true;

#ifdef _WIN32
if( config.StartupMode == "Service" )
{
wchar_t exePath[MAX_PATH] = {};
GetModuleFileNameW( nullptr, exePath, MAX_PATH );
success &= UpdateWindowsService( exePath, true );
}
#endif

std::string statusPath = configPath + ".status";
std::ofstream sf( statusPath );
if( sf )
{
crow::json::wvalue status;
status["success"] = success;
status["message"] = success ? "Configuration applied" : "Some operations failed";
sf << status.dump();
}

std::filesystem::remove( configPath );
return success ? 0 : 1;
}

return -1; // unrecognised command — fall through
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

#ifdef _WIN32
int wmain( int argc, wchar_t* argv[] )
#else
int main( int argc, char* argv[] )
#endif
{
// Normalize arguments to UTF-8 std::string on all platforms.
std::vector<std::string> args;
args.reserve( argc );
for( int i = 0; i < argc; i++ )
{
#ifdef _WIN32
int len = WideCharToMultiByte( CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr );
std::string s( len > 0 ? len - 1 : 0, '\0' );
WideCharToMultiByte( CP_UTF8, 0, argv[i], -1, s.data(), len, nullptr, nullptr );
args.push_back( std::move( s ) );
#else
args.emplace_back( argv[i] );
#endif
}

// Initialize logging early.
{
auto logDir = GetConfigFilePath( "logs" );
std::filesystem::create_directories( logDir );
Witness::LogInit( logDir.string(), Witness::LogLevel::Info );
}

// Handle CLI sub-commands first.
int cmdResult = HandleCliCommands( args );
if( cmdResult >= 0 )
return cmdResult;

// --- Start the server ---

#ifdef _WIN32
SERVICE_TABLE_ENTRYW services[] =
{
{ const_cast<LPWSTR>( SERVICE_NAME ), ServiceMain },
{ nullptr, nullptr }
};

if( !StartServiceCtrlDispatcherW( services ) )
{
DWORD err = GetLastError();
if( err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT )
{
// Running interactively (not as a Windows service).
s_WinNotifier.IsService = false;
SetConsoleCtrlHandler( ConsoleHandlerRoutine, TRUE );
RunServer();
return ReturnValue;
}
return (int)err;
}
return ReturnValue;

#else
// Linux: install signal handlers and run directly.
{
struct sigaction sa = {};
sa.sa_handler = PosixSignalHandler;
sigemptyset( &sa.sa_mask );
sa.sa_flags = SA_RESTART;
sigaction( SIGTERM, &sa, nullptr );
sigaction( SIGINT,  &sa, nullptr );
sigaction( SIGHUP,  &sa, nullptr );
}

static Witness::ServiceNotifier linuxNotifier;
const char* notifySock  = getenv( "NOTIFY_SOCKET" );
const char* invocId     = getenv( "INVOCATION_ID" );
linuxNotifier.IsService = ( notifySock && notifySock[0] ) ||
                          ( invocId   && invocId[0]   );
g_Notifier = &linuxNotifier;

linuxNotifier.NotifyStarting();
RunServer();
return ReturnValue;
#endif
}
