#include "Common.h"

bool GetSettingsString(const std::unordered_map< StringT, StringT >& Settings, const TCHAR* FieldName, StringT& OutString)
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
ValueType GetValueFromString(const StringT& ValueString);

template<>
StringT GetValueFromString(const StringT& ValueString)
{
	return ValueString;
}

template<>
double GetValueFromString(const StringT& ValueString)
{
	std::string Conv = StringToAnsi(ValueString);

	return atof( Conv.c_str() );
}


template<>
int GetValueFromString(const StringT& ValueString)
{
	std::string Conv = StringToAnsi(ValueString);

	return atoi(Conv.c_str());
}


template<>
bool GetValueFromString(const StringT& ValueString)
{
	return ValueString.compare(_T("True")) == 0;
}

#define BUILD_SETTINGS_FIELD_ACCESSOR( Type, TypeName ) \
	template<> \
	bool GetSettingsField( const std::unordered_map< StringT, StringT >& Settings, const TCHAR* FieldName, Type& ValueOut, StringT& Errors ) \
	{ \
		StringT Value; \
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

BUILD_SETTINGS_FIELD_ACCESSOR(StringT, _T("string"))
BUILD_SETTINGS_FIELD_ACCESSOR(double, _T("double"))
BUILD_SETTINGS_FIELD_ACCESSOR(int, _T("int"))
BUILD_SETTINGS_FIELD_ACCESSOR(bool, _T("bool"))
