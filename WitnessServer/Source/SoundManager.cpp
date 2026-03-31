#include "SoundManager.h"
#include <Log.h>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#else
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#endif

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

static void PlatformPlaySoundAsync( const std::string& soundFile )
{
#ifdef _WIN32
	PlaySoundA( soundFile.c_str(), nullptr, SND_FILENAME | SND_ASYNC );
#else
	// Fork a child process to play the .wav file; parent returns immediately.
	// Try paplay (PulseAudio / PipeWire-pulse) first, then aplay (ALSA).
	pid_t pid = fork();
	if( pid == 0 )
	{
		// Child: redirect stdout/stderr to /dev/null to stay silent
		int devnull = open( "/dev/null", O_WRONLY );
		if( devnull >= 0 )
		{
			dup2( devnull, STDOUT_FILENO );
			dup2( devnull, STDERR_FILENO );
			close( devnull );
		}
		execlp( "paplay", "paplay", soundFile.c_str(), (char*)nullptr );
		// paplay not found — try aplay
		execlp( "aplay", "aplay", "-q", soundFile.c_str(), (char*)nullptr );
		// Neither found — silently exit child
		_exit( 0 );
	}
	// Parent: don't wait — fire-and-forget
#endif
}

static void PlatformStopSound()
{
#ifdef _WIN32
	PlaySoundA( nullptr, nullptr, 0 );
#else
	// No reliable cross-platform "stop async sound"; nothing to do here.
#endif
}

static std::filesystem::path PlatformGetExeDir()
{
#ifdef _WIN32
	wchar_t exeBuf[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
	return std::filesystem::path( exeBuf ).parent_path();
#else
	return std::filesystem::canonical( "/proc/self/exe" ).parent_path();
#endif
}

// ---------------------------------------------------------------------------

bool SoundManager::TryPlaySound( int actionUID, int priority, int cooldownSeconds, const std::string& soundFile )
{
	std::lock_guard<std::mutex> lock( m_Mutex );

	auto now = std::chrono::steady_clock::now();

	// Check per-action cooldown
	auto it = m_LastTriggerTime.find( actionUID );
	if( it != m_LastTriggerTime.end() )
	{
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>( now - it->second ).count();
		if( elapsed < cooldownSeconds )
			return false;
	}

	// Check priority against currently playing sound.
	// PlaySoundA with SND_ASYNC returns immediately; we estimate ~3s max duration.
	// If a higher-priority sound is still "playing", skip.
	auto sincePlaying = std::chrono::duration_cast<std::chrono::seconds>( now - m_CurrentPlayStart ).count();
	bool soundStillPlaying = ( sincePlaying < 3 );

	if( soundStillPlaying && priority <= m_CurrentPriority )
		return false;

	// If preempting a lower-priority sound, stop it first
	if( soundStillPlaying && priority > m_CurrentPriority )
	{
		PlatformStopSound();
	}

	PlatformPlaySoundAsync( soundFile );

	m_CurrentPriority = priority;
	m_CurrentPlayStart = now;
	m_LastTriggerTime[actionUID] = now;

	return true;
}

std::string SoundManager::ResolveSoundPath( const std::string& param )
{
	std::filesystem::path soundPath( param );
	if( soundPath.is_relative() )
	{
		soundPath = PlatformGetExeDir() / soundPath;
	}
	return soundPath.string();
}
