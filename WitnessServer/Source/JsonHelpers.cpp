#include "Common.h"
#include "cpprest/json.h"

using namespace std;
using namespace web;
using namespace utility;

void ReportJsonError(const json::value& Object, const TCHAR* FieldName, string_t& Errors, const TCHAR* FieldType)
{
	if (Object.has_field(FieldName))
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

BUILD_JSON_FIELD_ACCESSOR(as_string, is_string, string_t, _T("string"))
BUILD_JSON_FIELD_ACCESSOR(as_double, is_double, double, _T("double"))
BUILD_JSON_FIELD_ACCESSOR(as_integer, is_integer, int, _T("int"))
BUILD_JSON_FIELD_ACCESSOR(as_bool, is_boolean, bool, _T("bool"))

bool GetSettingsString(const std::unordered_map< string_t, string_t >& Settings, const TCHAR* FieldName, string_t& OutString)
{
	auto Iter = Settings.find(FieldName);
	if (Iter != Settings.end())
	{
		OutString = (*Iter).second;
		return true;
	}
	return false;
}

template<typename ValueType>
ValueType GetValueFromString(const string_t& ValueString);

template<>
string_t GetValueFromString(const string_t& ValueString)
{
	return ValueString;
}

template<>
double GetValueFromString(const string_t& ValueString)
{
	string Conv(ValueString.begin(), ValueString.end());

	return atof( Conv.c_str() );
}


template<>
int GetValueFromString(const string_t& ValueString)
{
	string Conv(ValueString.begin(), ValueString.end());

	return atoi(Conv.c_str());
}


template<>
bool GetValueFromString(const string_t& ValueString)
{
	return ValueString.compare(_T("True")) == 0;
}

#define BUILD_SETTINGS_FIELD_ACCESSOR( Type, TypeName ) \
	template<> \
	bool GetSettingsField( const std::unordered_map< string_t, string_t >& Settings, const TCHAR* FieldName, Type& ValueOut, string_t& Errors ) \
	{ \
		string_t Value; \
		if( GetSettingsString( Settings, FieldName, Value ) ) \
		{ \
			ValueOut = GetValueFromString<Type>(Value); \
			return true; \
		} \
		else \
		{ \
			Errors += _T("Field "); \
			Errors += FieldName; \
			Errors += _T(" was not a "); \
			Errors += TypeName; \
			Errors += _T(".\n"); \
			return false; \
		} \
	}

BUILD_SETTINGS_FIELD_ACCESSOR(string_t, _T("string"))
BUILD_SETTINGS_FIELD_ACCESSOR(double, _T("double"))
BUILD_SETTINGS_FIELD_ACCESSOR(int, _T("int"))
BUILD_SETTINGS_FIELD_ACCESSOR(bool, _T("bool"))
