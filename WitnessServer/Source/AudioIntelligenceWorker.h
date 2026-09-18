#pragma once

#include "WorkerBase.h"

#include <AudioClassifier.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SQLiteDatabase;
class GlobalContext;

// Shared by the production worker and the command-line regression probe.
bool DecodeAudioForIntelligence( const std::string& Path, std::vector<float>& Samples );

class AudioIntelligenceWorker : public WorkerBase
{
public:
	AudioIntelligenceWorker( const std::shared_ptr<MessageBus>& MessageBus,
		std::shared_ptr<SQLiteDatabase> Database,
		std::shared_ptr<Witness::Camera::AudioClassifier> Classifier,
		std::string CachePath,
		std::shared_ptr<GlobalContext> Context,
		std::function<bool()> IsIdle );

private:
	void WorkerMain() override;
	void MarkProcessed( int64_t ClipUID );
	void ProcessClip( int64_t ClipUID, int64_t Timestamp, int Camera,
		int RecordMode, float Threshold );

	std::shared_ptr<SQLiteDatabase> Database;
	std::shared_ptr<Witness::Camera::AudioClassifier> Classifier;
	std::string CachePath;
	std::shared_ptr<GlobalContext> Context;
	std::function<bool()> IsIdle;
};
