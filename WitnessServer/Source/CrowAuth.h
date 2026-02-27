#pragma once

#include <string>
#include "crow.h"
#include "Common.h"
#include "GlobalContext.h"

namespace CrowAuth
{
	enum class Action { Read, ReadWrite };
	enum class Privilege { Normal, Administrator };

	// Extract session token from cookie header
	std::string GetSessionToken( const crow::request& req, uint16_t port );

	// Returns UserUID (>= 0) on success, -1 on failure
	int IsAuthenticated( const GlobalContext& Context, const crow::request& req, const crow::json::rvalue* body,
		Action actionType, Privilege requiredPrivilege );

	// Returns UserUID (> 0) on success, 0 on failure
	int IsCameraAuthenticated( const GlobalContext& Context, const crow::request& req, const crow::json::rvalue* body,
		Action actionType, Privilege requiredPrivilege, int cameraUID );
}
