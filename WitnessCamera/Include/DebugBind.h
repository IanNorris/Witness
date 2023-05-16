#pragma once

#include "Export.h"

#include <string>
#include <mutex>
#include <vector>
#include <algorithm>

class DebugConsole;

class CAMERA_API DebugBindBase
{
public:
	DebugBindBase( DebugConsole* Base, const char* Name );

	virtual std::string Get() const = 0;
	virtual bool Set( const char* Value ) = 0;
	virtual void Reset() = 0;

	virtual ~DebugBindBase();

	const char* GetName() { return m_Name; }

private:

	DebugConsole* m_Parent;
	const char* m_Name;
};

class DebugConsole
{
public:
	virtual void Register( DebugBindBase* Object )
	{
		std::unique_lock<std::mutex> Lock(Mutex);

		Values.push_back(Object);
	}

	virtual void Unregister( DebugBindBase* Object )
	{
		std::unique_lock<std::mutex> Lock(Mutex);

		Values.erase(std::remove_if(Values.begin(), Values.end(), [=]( DebugBindBase* Other) { return Other == Object; }));
	}
	
	virtual ~DebugConsole() {}

	const std::vector<DebugBindBase*> GetValues()
	{
		std::unique_lock<std::mutex> Lock(Mutex);

		return Values;
	}

private:

	std::mutex Mutex;
	std::vector<DebugBindBase*> Values;
};

template<typename T>
class CAMERA_API DebugBind : public DebugBindBase
{
public:
	DebugBind( DebugConsole* Base, const char* Name, T* Data )
		: DebugBindBase( Base, Name )
		, m_Data( Data )
		, m_Original( *Data )
	{}

	virtual std::string Get() const;
	virtual bool Set( const char* Value );
	virtual void Reset() { *m_Data = m_Original; }

private:

	T* m_Data;
	T m_Original;
};
