#pragma once

#include "Common.h"

class GlobalContext;

std::string GetRandomToken();
std::string GetHashedPasswordKey_Algorithm0( const std::string& Username, const std::string Password );
bool CheckHashedPasswordKey_Algorithm0( const std::string& Key, const std::string& Username, const std::string Password );
void OfflineCreationForFirstUser( const GlobalContext& Context );
