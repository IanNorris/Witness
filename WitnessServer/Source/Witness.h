#pragma once

class WitnessListener;
class GlobalContext;
class MessageBusQueue;

struct AndroidSettings
{
	AndroidSettings()
	: UseAndroid(false)
	{}

	string_t	ServerKey;
	string_t	TempUserId;
	bool		UseAndroid;
};

class WitnessServer
{
public:

	bool Initialize();

	void MessageLoop();

	void Shutdown();

private:

	void LoadAndroidSettings( const json::value& JsonAndroidSettings );
	bool CreateListener( const json::value& JsonServerSettings );
	bool InitializeContext();

	void StartCameraWorkers();

	unique_ptr<WitnessListener>	Server;
	shared_ptr<GlobalContext> Context;
	shared_ptr<MessageBusQueue> MessageClient;

	AndroidSettings	Android;

	string_t CachePath;
};
