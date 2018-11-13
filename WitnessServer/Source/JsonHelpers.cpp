#include "Common.h"
#include "cpprest/json.h"

using namespace std;
using namespace web;
using namespace utility;

void ReportJsonError( const json::value& Object, const TCHAR* FieldName, string_t& Errors, const TCHAR* FieldType )
{
	if( Object.has_field( FieldName ) )
	{
		Errors += _T("Field ");
		Errors += FieldName;
		Errors += _T(" was not found.\n");
	}
	else
	{
		Errors += _T("Field ");
		Errors += FieldName;
		Errors += _T(" was not a ");
		Errors += FieldType;
		Errors += _T(".\n");
	}
}

#define BUILD_JSON_FIELD_ACCESSOR( AsFunction, IsFunction, Type, TypeName ) \
	template<> \
	bool GetJsonField( const json::value& Object, const TCHAR* FieldName, Type& ValueOut, string_t& Errors ) \
	{ \
		if( Object.has_field( FieldName ) && Object.at(FieldName). IsFunction() ) \
		{ \
			ValueOut = Object.at( FieldName ). AsFunction (); \
			return true; \
		} \
		else \
		{ \
			ReportJsonError( Object, FieldName, Errors, TypeName ); \
			return false; \
		} \
	}

BUILD_JSON_FIELD_ACCESSOR( as_string, is_string, string_t, _T("string") )
BUILD_JSON_FIELD_ACCESSOR( as_double, is_double, double, _T("double") )
BUILD_JSON_FIELD_ACCESSOR( as_integer, is_integer, int, _T("int") )
BUILD_JSON_FIELD_ACCESSOR( as_bool, is_boolean, bool, _T("bool") )
