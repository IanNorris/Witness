#include "Common.h"

bool GetSettingsString(const std::unordered_map< std::string, std::string >& Settings, const char* FieldName, std::string& OutString)
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
ValueType GetValueFromString(const std::string& ValueString);

template<>
std::string GetValueFromString(const std::string& ValueString)
{
	return ValueString;
}

template<>
double GetValueFromString(const std::string& ValueString)
{
	return atof( ValueString.c_str() );
}


template<>
int GetValueFromString(const std::string& ValueString)
{
	return atoi(ValueString.c_str());
}


template<>
bool GetValueFromString(const std::string& ValueString)
{
	return ValueString.compare("True") == 0;
}

#define BUILD_SETTINGS_FIELD_ACCESSOR( Type, TypeName ) \
	template<> \
	bool GetSettingsField( const std::unordered_map< std::string, std::string >& Settings, const char* FieldName, Type& ValueOut, std::string& Errors ) \
	{ \
		std::string Value; \
		if( GetSettingsString( Settings, FieldName, Value ) ) \
		{ \
			ValueOut = GetValueFromString<Type>(Value); \
			return true; \
		} \
		else \
		{ \
			Errors += "Field "; \
			Errors += FieldName; \
			Errors += " was not a "; \
			Errors += TypeName; \
			Errors += ".\n"; \
			return false; \
		} \
	}

BUILD_SETTINGS_FIELD_ACCESSOR(std::string, "string")
BUILD_SETTINGS_FIELD_ACCESSOR(double, "double")
BUILD_SETTINGS_FIELD_ACCESSOR(int, "int")
BUILD_SETTINGS_FIELD_ACCESSOR(bool, "bool")
