#include "CrowAuth.h"

std::string CrowAuth::GetSessionToken( const crow::request& req, uint16_t port )
{
	std::string PortName = std::to_string( port );
	std::string SessionTokenName = "SessionToken-" + PortName;

	std::string SessionToken;

	auto CookieHeader = req.get_header_value( "Cookie" );
	if( CookieHeader.empty() )
		return SessionToken;

	auto CookieSplit = SplitString( CookieHeader, ";", StringTrim::Trim, StringStrip::RemoveEmpty );
	for( const auto& Cookie : CookieSplit )
	{
		auto TokenSplit = SplitString( Cookie, "=", StringTrim::Trim, StringStrip::DoNotRemoveEmpty );
		if( TokenSplit.size() == 2 )
		{
			std::string Left = Trim( TokenSplit[0] );
			std::string Right = Trim( TokenSplit[1] );

			if( Left == SessionTokenName )
			{
				SessionToken = Right;
			}
		}
	}

	return SessionToken;
}

int CrowAuth::IsAuthenticated( const GlobalContext& Context, const crow::request& req, const crow::json::rvalue* body,
	Action actionType, Privilege requiredPrivilege )
{
	// Extract CSRF from body for R/W actions
	std::string CSRF;
	if( actionType == Action::ReadWrite )
	{
		if( !body || !body->has( "csrf" ) )
			return -1;

		std::string csrfStr = (*body)["csrf"].s();
		CSRF = std::string( csrfStr.begin(), csrfStr.end() );
	}

	std::string SessionTokenStr = GetSessionToken( req, Context.Port );
	std::string SessionToken = std::string( SessionTokenStr.begin(), SessionTokenStr.end() );
	int UserUID = -1;

	{
		bool QuerySuccess = false;

		if( actionType == Action::ReadWrite )
		{
			SQLiteDatabaseQueryInstance VerifySessionAndCSRF( Context.Database, "VerifySessionAndCSRF" );
			VerifySessionAndCSRF->Bind( "@SessionToken", SessionToken.c_str() );
			VerifySessionAndCSRF->Bind( "@CSRFToken", CSRF.c_str() );

			int Count = VerifySessionAndCSRF->Execute(
				[&]( const SQLiteDatabaseQuery& query )
				{
					UserUID = query.GetColumnValueInt( 0 );
					QuerySuccess = true;
					return true;
				}
			);
			if( Count != 1 || !QuerySuccess )
				return -1;
		}
		else
		{
			SQLiteDatabaseQueryInstance VerifySession( Context.Database, "VerifySession" );
			VerifySession->Bind( "@SessionToken", SessionToken.c_str() );

			int Count = VerifySession->Execute(
				[&]( const SQLiteDatabaseQuery& query )
				{
					UserUID = query.GetColumnValueInt( 0 );
					QuerySuccess = true;
					return true;
				}
			);
			if( Count != 1 || !QuerySuccess )
				return -1;
		}
	}

	int Enabled = 0;
	int Admin = 0;

	{
		SQLiteDatabaseQueryInstance FindUserForAuth( Context.Database, "FindUserForAuth" );
		FindUserForAuth->Bind( "@UserUID", UserUID );

		bool Success = false;
		FindUserForAuth->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				Enabled = query.GetColumnValueInt( 3 );
				Admin = query.GetColumnValueInt( 4 );
				Success = true;
				return true;
			}
		);
	}

	if( !Admin && requiredPrivilege == Privilege::Administrator )
		return -1;

	if( !Enabled )
		return -1;

	return UserUID;
}

int CrowAuth::IsCameraAuthenticated( const GlobalContext& Context, const crow::request& req, const crow::json::rvalue* body,
	Action actionType, Privilege requiredPrivilege, int cameraUID )
{
	int UserUID = IsAuthenticated( Context, req, body, actionType, requiredPrivilege );
	if( UserUID < 0 )
		return 0;

	SQLiteDatabaseQueryInstance GetCamerasDetailsForUser( Context.Database, "GetCamerasDetailsForUser" );
	GetCamerasDetailsForUser->Bind( "@User", UserUID );
	GetCamerasDetailsForUser->Bind( "@Camera", cameraUID );

	bool Success = false;
	GetCamerasDetailsForUser->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			Success = true;
			return true;
		}
	);

	return Success ? UserUID : 0;
}
