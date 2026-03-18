#pragma once

#include <string>
#include <mutex>
#include <unordered_map>
#include <chrono>

// Global sound manager with priority-based preemption and per-action cooldowns.
// Thread-safe — called from multiple CameraWorker threads and MotionEvents.
class SoundManager
{
public:
	// Attempt to play a sound. Returns true if the sound was played.
	// - actionUID: unique ID of the action (for cooldown tracking)
	// - priority: higher values preempt lower (0-100)
	// - cooldownSeconds: minimum seconds between triggers of the same action
	// - soundFile: absolute path to .wav file
	bool TryPlaySound( int actionUID, int priority, int cooldownSeconds, const std::string& soundFile );

	// Resolve a potentially relative sound path to absolute (relative to exe dir)
	static std::string ResolveSoundPath( const std::string& param );

private:
	std::mutex m_Mutex;

	// Per-action cooldown tracking (actionUID -> last trigger time)
	std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastTriggerTime;

	// Currently playing sound state
	int m_CurrentPriority = -1;
	std::chrono::steady_clock::time_point m_CurrentPlayStart;
};
