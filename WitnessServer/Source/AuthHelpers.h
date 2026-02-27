#pragma once

#include "Common.h"

class GlobalContext;

StringT GetRandomToken();
StringT GetHashedPasswordKey_Algorithm0( const StringT& Username, const StringT Password );
bool CheckHashedPasswordKey_Algorithm0( const StringT& Key, const StringT& Username, const StringT Password );
void OfflineCreationForFirstUser( const GlobalContext& Context );
