#pragma once

#include "DebugBind.h"

DebugBindBase::DebugBindBase( DebugConsole* Base, const char* Name )
: m_Parent( Base )
, m_Name( Name )
{
	if( m_Parent )
	{
		m_Parent->Register( this );
	}
}

DebugBindBase::~DebugBindBase()
{
	if( m_Parent )
	{
		m_Parent->Unregister( this );
	}
}


std::string DebugBind<unsigned int>::Get() const
{
	return std::to_string(*m_Data);
}

bool DebugBind<unsigned int>::Set( const char* Value )
{
	try
	{
		*m_Data = std::stoul( Value );
		return true;
	}
	catch(std::invalid_argument)
	{
		return false;
	}
}

std::string DebugBind<int>::Get() const
{
	return std::to_string(*m_Data);
}

bool DebugBind<int>::Set( const char* Value )
{
	try
	{
		*m_Data = std::stol( Value );
		return true;
	}
	catch(std::invalid_argument)
	{
		return false;
	}
}

std::string DebugBind<float>::Get() const
{
	return std::to_string(*m_Data);
}

bool DebugBind<float>::Set( const char* Value )
{
	try
	{
		*m_Data = std::stof( Value );
		return true;
	}
	catch(std::invalid_argument)
	{
		return false;
	}
}

std::string DebugBind<double>::Get() const
{
	return std::to_string(*m_Data);
}

bool DebugBind<double>::Set( const char* Value )
{
	try
	{
		*m_Data = std::stof( Value );
		return true;
	}
	catch(std::invalid_argument)
	{
		return false;
	}
}
