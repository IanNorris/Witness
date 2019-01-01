#pragma once

#include <string>
#include <vector>
#include <algorithm>

class DebugConsole;

class DebugBindBase
{
public:
	DebugBindBase( DebugConsole* Base, const char* Name );

	virtual std::string Get() const = 0;
	virtual bool Set( const std::string& Value ) = 0;
	virtual void Reset() = 0;

	virtual ~DebugBindBase();

	const std::string& GetName() { return m_Name; }

private:

	DebugConsole* m_Parent;
	const std::string m_Name;
};

class DebugConsole
{
public:
	virtual void Register( DebugBindBase* Object )
	{
		Values.push_back(Object);
	}

	virtual void Unregister( DebugBindBase* Object )
	{
		std::remove_if(Values.begin(), Values.end(), [=]( DebugBindBase* Other) { return Other == Object; });
	}
	
	virtual ~DebugConsole() {}

	const std::vector<DebugBindBase*> GetValues()
	{
		return Values;
	}

private:

	std::vector<DebugBindBase*> Values;
};

template<typename T>
class DebugBind : public DebugBindBase
{
public:
	DebugBind( DebugConsole* Base, const char* Name, T* Data )
		: DebugBindBase( Base, Name )
		, m_Data( Data )
		, m_Original( *Data )
	{}

	virtual std::string Get() const;
	virtual bool Set( const std::string& Value );
	virtual void Reset() { *m_Data = m_Original; }

private:

	T* m_Data;
	T m_Original;
};
