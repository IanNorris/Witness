#include "SoundManager.h"
#include <Log.h>
#include <filesystem>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")

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
		PlaySoundA( nullptr, nullptr, 0 );
	}

	// Play the new sound
	PlaySoundA( soundFile.c_str(), nullptr, SND_FILENAME | SND_ASYNC );

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
		wchar_t exeBuf[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
		soundPath = std::filesystem::path( exeBuf ).parent_path() / soundPath;
	}
	return soundPath.string();
}
