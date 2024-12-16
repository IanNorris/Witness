#pragma once

#include <functional>

struct Message
{
public:

	virtual ~Message() {}

	template< typename T > void Handle( const std::function< void(const T&) >& Delegate )
	{
		T* Object = dynamic_cast<T*>(this);
		if( Object )
		{
			Delegate( *Object );
		}
	}
};

struct ThreadShutdownMessage : public Message
{
};