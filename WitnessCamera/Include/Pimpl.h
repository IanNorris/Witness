#pragma once

#define PIMPL_CONSTRUCT(Type)\
template<> Type* Construct()\
{return new Type();}\
template<> void Destruct( Type* Data )\
{delete Data;}

namespace Witness{
namespace Camera{

template<typename T> extern T* Construct();
template<typename T> extern void Destruct( T* Data );

template<typename T>
class Pimpl
{
public:
	Pimpl()
	: m_InternalData( Construct<T>() )
	{
	
	}
	
	virtual ~Pimpl()
	{
		Destruct<T>( m_InternalData );
		m_InternalData = nullptr;
	}

	T& GetData()
	{
		return *m_InternalData;
	}

protected:

	T* m_InternalData;
};

}}
