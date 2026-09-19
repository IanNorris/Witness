#include "CrowListener.h"
#include "CrowAuth.h"
#include "AuthHelpers.h"
#include "SQLite.h"
#include "TagHelpers.h"

#include <sodium.h>
#include <asio/ip/address.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>

namespace
{
	constexpr int ScopeHealth = 1;
	constexpr int ScopeClips = 2;
	constexpr int ScopeRecord = 4;

	int64_t NowSeconds()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	std::string ApiTrim( std::string Value )
	{
		const auto First = Value.find_first_not_of( " \t\r\n" );
		if( First == std::string::npos ) return {};
		const auto Last = Value.find_last_not_of( " \t\r\n" );
		return Value.substr( First, Last - First + 1 );
	}

	std::string HashKey( const std::string& Key )
	{
		unsigned char Digest[crypto_generichash_BYTES];
		crypto_generichash( Digest, sizeof( Digest ),
			reinterpret_cast<const unsigned char*>( Key.data() ), Key.size(), nullptr, 0 );
		char Hex[sizeof( Digest ) * 2 + 1];
		sodium_bin2hex( Hex, sizeof( Hex ), Digest, sizeof( Digest ) );
		return Hex;
	}

	bool ParseCIDR( const std::string& Text, asio::ip::address& Address, unsigned& Prefix )
	{
		const auto Slash = Text.find( '/' );
		const auto Host = ApiTrim( Text.substr( 0, Slash ) );
		try { Address = asio::ip::make_address( Host ); }
		catch( ... ) { return false; }
		const unsigned Maximum = Address.is_v4() ? 32 : 128;
		if( Slash == std::string::npos ) { Prefix = Maximum; return true; }
		const auto Suffix = Text.substr( Slash + 1 );
		if( Suffix.empty() || !std::all_of( Suffix.begin(), Suffix.end(),
			[]( unsigned char Ch ) { return std::isdigit( Ch ) != 0; } ) ) return false;
		try { Prefix = std::stoul( Suffix ); }
		catch( ... ) { return false; }
		return Prefix <= Maximum;
	}

	bool AddressInCIDR( const asio::ip::address& Peer, const asio::ip::address& Network, unsigned Prefix )
	{
		if( Peer.is_v4() != Network.is_v4() ) return false;
		if( Peer.is_v4() )
		{
			const uint32_t Mask = Prefix ? (0xffffffffu << (32 - Prefix)) : 0;
			return (Peer.to_v4().to_uint() & Mask) == (Network.to_v4().to_uint() & Mask);
		}
		const auto PeerBytes = Peer.to_v6().to_bytes();
		const auto NetworkBytes = Network.to_v6().to_bytes();
		for( size_t Index = 0; Index < PeerBytes.size(); ++Index )
		{
			const unsigned Bits = std::min( Prefix, 8u );
			const unsigned Mask = Bits ? (0xffu << (8 - Bits)) : 0;
			if( (PeerBytes[Index] & Mask) != (NetworkBytes[Index] & Mask) ) return false;
			Prefix -= Bits;
		}
		return true;
	}

	bool ValidCIDRList( const std::string& List, const std::string* Peer = nullptr )
	{
		if( List.empty() || List.size() > 1024 ) return false;
		asio::ip::address PeerAddress;
		if( Peer )
		{
			try { PeerAddress = asio::ip::make_address( *Peer ); }
			catch( ... ) { return false; }
		}
		bool Match = false;
		std::istringstream Input( List );
		std::string Entry;
		while( std::getline( Input, Entry, ',' ) )
		{
			asio::ip::address Network;
			unsigned Prefix = 0;
			if( !ParseCIDR( ApiTrim( Entry ), Network, Prefix ) ) return false;
			if( Peer && AddressInCIDR( PeerAddress, Network, Prefix ) ) Match = true;
		}
		return Peer ? Match : true;
	}

	bool ParseUnsigned( const char* Text, int64_t& Value, int64_t Maximum )
	{
		if( !Text || !*Text ) return false;
		Value = 0;
		for( const unsigned char* P = reinterpret_cast<const unsigned char*>( Text ); *P; ++P )
		{
			if( !std::isdigit( *P ) || Value > (Maximum - (*P - '0')) / 10 ) return false;
			Value = Value * 10 + *P - '0';
		}
		return true;
	}

	void JsonReply( crow::response& Res, crow::json::wvalue&& Value, int Code = 200 )
	{
		Res.set_header( "Content-Type", "application/json" );
		Res.set_header( "Cache-Control", "no-store" );
		Res.body = Value.dump();
		Res.code = Code;
		Res.end();
	}
}

bool CrowListener::AuthorizeApiKey( const crow::request& req, int scope, int& ownerUserUID )
{
	ownerUserUID = -1;
	// Never honour forwarding headers: the allowlist is for the actual socket peer.
	if( req.headers.count( "Forwarded" ) || req.headers.count( "X-Forwarded-For" ) ||
		req.headers.count( "X-Real-IP" ) ) return false;
	const std::string Header = req.get_header_value( "Authorization" );
	if( Header.size() != 75 || Header.compare( 0, 11, "Bearer wtn_" ) != 0 ) return false;
	if( !std::all_of( Header.begin() + 11, Header.end(),
		[]( unsigned char Ch ) { return std::isxdigit( Ch ) != 0; } ) ) return false;
	const std::string Hash = HashKey( Header.substr( 7 ) );
	int KeyUID = 0, ScopeMask = 0;
	std::string CIDRs;
	{
		SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "FindApiKey" );
		Query->Bind( "@Hash", Hash.c_str() );
		Query->Execute( [&]( const SQLiteDatabaseQuery& Row )
		{
			KeyUID = Row.GetColumnValueInt( 0 );
			ownerUserUID = Row.GetColumnValueInt( 1 );
			ScopeMask = Row.GetColumnValueInt( 2 );
			CIDRs = Row.GetColumnValueText( 3 );
			return true;
		} );
	}
	if( !KeyUID || !(ScopeMask & scope) || !ValidCIDRList( CIDRs, &req.remote_ip_address ) )
	{
		ownerUserUID = -1;
		return false;
	}
	const int64_t Now = NowSeconds();
	{
		SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "TouchApiKey" );
		Query->Bind( "@KeyUID", KeyUID );
		Query->Bind( "@Now", Now );
		Query->Bind( "@IP", req.remote_ip_address.c_str() );
		Query->Execute( nullptr );
	}
	{
		SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "AuditApiKey" );
		Query->Bind( "@KeyUID", KeyUID );
		Query->Bind( "@Now", Now );
		Query->Bind( "@Scope", scope );
		Query->Bind( "@IP", req.remote_ip_address.c_str() );
		Query->Bind( "@Path", req.url.c_str() );
		Query->Execute( nullptr );
	}
	return true;
}

void CrowListener::HandleApiKeyCreate( const crow::request& req, crow::response& res )
{
	const auto Body = crow::json::load( req.body );
	if( !Body ) { res.code = 400; res.end(); return; }
	const int Owner = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &Body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( Owner < 0 ) { res.code = 403; res.end(); return; }
	if( !Body.has( "name" ) || !Body.has( "scopes" ) || !Body.has( "allowedCidrs" ) ||
		Body["name"].t() != crow::json::type::String ||
		Body["scopes"].t() != crow::json::type::List ||
		Body["allowedCidrs"].t() != crow::json::type::String )
	{ res.code = 400; res.end(); return; }
	const std::string Name = ApiTrim( Body["name"].s() );
	const std::string CIDRs = ApiTrim( Body["allowedCidrs"].s() );
	if( Name.empty() || Name.size() > 128 || !ValidCIDRList( CIDRs ) )
	{ res.code = 400; res.end(); return; }
	int ScopeMask = 0;
	for( size_t Index = 0; Index < Body["scopes"].size(); ++Index )
	{
		if( Body["scopes"][Index].t() != crow::json::type::String )
		{ res.code = 400; res.end(); return; }
		const std::string Scope = Body["scopes"][Index].s();
		if( Scope == "health.read" ) ScopeMask |= ScopeHealth;
		else if( Scope == "clips.read" ) ScopeMask |= ScopeClips;
		else if( Scope == "record.trigger" ) ScopeMask |= ScopeRecord;
		else { res.code = 400; res.end(); return; }
	}
	if( !ScopeMask ) { res.code = 400; res.end(); return; }
	const std::string Key = "wtn_" + GetRandomToken();
	const std::string Hash = HashKey( Key );
	int64_t KeyUID = 0;
	{
		SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "CreateApiKey" );
		Query->Bind( "@Name", Name.c_str() );
		Query->Bind( "@KeyHash", Hash.c_str() );
		Query->Bind( "@Owner", Owner );
		Query->Bind( "@Scopes", ScopeMask );
		Query->Bind( "@CIDRs", CIDRs.c_str() );
		Query->Bind( "@Now", NowSeconds() );
		Query->Execute( nullptr );
		KeyUID = Query->GetLastInsertionId();
	}
	if( KeyUID <= 0 ) { res.code = 500; res.end(); return; }
	crow::json::wvalue Result;
	Result["id"] = KeyUID;
	Result["key"] = Key; // Returned exactly once; only its hash is stored.
	JsonReply( res, std::move( Result ), 201 );
}

void CrowListener::HandleApiKeyList( const crow::request& req, crow::response& res )
{
	if( CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator ) < 0 )
	{ res.code = 403; res.end(); return; }
	std::vector<crow::json::wvalue> Keys;
	SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "ListApiKeys" );
	Query->Execute( [&]( const SQLiteDatabaseQuery& Row )
	{
		crow::json::wvalue Key;
		Key["id"] = Row.GetColumnValueInt( 0 );
		Key["name"] = Row.GetColumnValueText( 1 );
		Key["ownerUserId"] = Row.GetColumnValueInt( 2 );
		Key["scopeMask"] = Row.GetColumnValueInt( 3 );
		Key["allowedCidrs"] = Row.GetColumnValueText( 4 );
		Key["createdAt"] = Row.GetColumnValueInt64( 5 );
		Key["lastUsedAt"] = Row.GetColumnValueInt64( 6 );
		Key["lastSourceIp"] = Row.GetColumnValueText( 7 ) ? Row.GetColumnValueText( 7 ) : "";
		Key["useCount"] = Row.GetColumnValueInt64( 8 );
		Key["revokedAt"] = Row.GetColumnValueInt64( 9 );
		Keys.push_back( std::move( Key ) );
		return true;
	} );
	crow::json::wvalue Result;
	Result["keys"] = std::move( Keys );
	JsonReply( res, std::move( Result ) );
}

void CrowListener::HandleApiKeyRevoke( const crow::request& req, crow::response& res )
{
	const auto Body = crow::json::load( req.body );
	if( !Body ) { res.code = 400; res.end(); return; }
	if( CrowAuth::IsAuthenticated( *m_GlobalContext, req, &Body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator ) < 0 )
	{ res.code = 403; res.end(); return; }
	if( !Body.has( "id" ) || Body["id"].t() != crow::json::type::Number || Body["id"].i() <= 0 )
	{ res.code = 400; res.end(); return; }
	SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "RevokeApiKey" );
	Query->Bind( "@KeyUID", (int64_t)Body["id"].i() );
	Query->Bind( "@Now", NowSeconds() );
	Query->Execute( nullptr );
	crow::json::wvalue Result;
	Result["revoked"] = true;
	JsonReply( res, std::move( Result ) );
}

void CrowListener::HandleApiKeyAudit( const crow::request& req, crow::response& res )
{
	if( CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator ) < 0 )
	{ res.code = 403; res.end(); return; }
	std::vector<crow::json::wvalue> Events;
	SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "ListApiKeyAudit" );
	Query->Execute( [&]( const SQLiteDatabaseQuery& Row )
	{
		crow::json::wvalue Event;
		Event["keyId"] = Row.GetColumnValueInt( 0 );
		Event["timestamp"] = Row.GetColumnValueInt64( 1 );
		Event["scopeMask"] = Row.GetColumnValueInt( 2 );
		Event["sourceIp"] = Row.GetColumnValueText( 3 );
		Event["path"] = Row.GetColumnValueText( 4 );
		Events.push_back( std::move( Event ) );
		return true;
	} );
	crow::json::wvalue Result;
	Result["events"] = std::move( Events );
	JsonReply( res, std::move( Result ) );
}

void CrowListener::HandleApiClipSearch( const crow::request& req, crow::response& res )
{
	int Owner = -1;
	if( !AuthorizeApiKey( req, ScopeClips, Owner ) ) { res.code = 401; res.end(); return; }
	int64_t From = 0, To = NowSeconds(), Camera = -1, Limit = 50, Offset = 0, MinDuration = 0;
	auto ReadParam = [&]( const char* Name, int64_t& Target, int64_t Maximum )
	{
		const char* Value = req.url_params.get( Name );
		return !Value || ParseUnsigned( Value, Target, Maximum );
	};
	if( !ReadParam( "from", From, INT64_MAX ) || !ReadParam( "to", To, INT64_MAX ) ||
		!ReadParam( "limit", Limit, 100 ) || !ReadParam( "offset", Offset, 1000000 ) ||
		!ReadParam( "minDuration", MinDuration, 86400 ) || From > To || Limit == 0 )
	{ res.code = 400; res.end(); return; }
	if( const char* CameraText = req.url_params.get( "camera" ) )
	{
		if( !ParseUnsigned( CameraText, Camera, 1000000000 ) || Camera == 0 )
		{ res.code = 400; res.end(); return; }
	}
	const std::string Tag = req.url_params.get( "tag" ) ? req.url_params.get( "tag" ) : "";
	if( Tag.size() > 128 ) { res.code = 400; res.end(); return; }
	std::vector<crow::json::wvalue> Clips;
	SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "SelectApiClips" );
	Query->Bind( "@From", From );
	Query->Bind( "@To", To );
	Query->Bind( "@Camera", (int)Camera );
	Query->Bind( "@MinDuration", (int)MinDuration );
	Query->Bind( "@Tag", Tag.c_str() );
	Query->Bind( "@User", Owner );
	Query->Bind( "@Limit", (int)Limit );
	Query->Bind( "@Offset", (int)Offset );
	Query->Execute( [&]( const SQLiteDatabaseQuery& Row )
	{
		crow::json::wvalue Clip;
		Clip["id"] = Row.GetColumnValueInt64( 0 );
		Clip["timestamp"] = Row.GetColumnValueInt64( 1 );
		Clip["cameraId"] = Row.GetColumnValueInt( 2 );
		Clip["duration"] = Row.GetColumnValueInt( 3 );
		Clip["activeDuration"] = Row.GetColumnValueInt( 4 );
		Clip["recordMode"] = Row.GetColumnValueInt( 5 );
		Clip["maxMotion"] = Row.GetColumnValueDouble( 6 );
		Clip["saved"] = Row.GetColumnValueInt( 7 ) != 0;
		Clip["description"] = Row.GetColumnValueText( 8 ) ? Row.GetColumnValueText( 8 ) : "";
		Clip["tags"] = Row.GetColumnValueText( 9 ) ? Row.GetColumnValueText( 9 ) : "";
		Clips.push_back( std::move( Clip ) );
		return true;
	} );
	crow::json::wvalue Result;
	Result["clips"] = std::move( Clips );
	Result["offset"] = Offset;
	Result["limit"] = Limit;
	JsonReply( res, std::move( Result ) );
}

void CrowListener::HandleApiCameraRecord( const crow::request& req, crow::response& res, int cameraId )
{
	int Owner = -1;
	if( !AuthorizeApiKey( req, ScopeRecord, Owner ) ) { res.code = 401; res.end(); return; }
	bool CameraAllowed = false;
	{
		SQLiteDatabaseQueryInstance Query( m_GlobalContext->Database, "GetCamerasDetailsForUser" );
		Query->Bind( "@User", Owner );
		Query->Bind( "@Camera", cameraId );
		Query->Execute( [&]( const SQLiteDatabaseQuery& ) { CameraAllowed = true; return true; } );
	}
	if( !CameraAllowed ) { res.code = 403; res.end(); return; }
	const auto Body = crow::json::load( req.body );
	if( !Body || !Body.has( "record" ) || Body["record"].t() != crow::json::type::True &&
		Body["record"].t() != crow::json::type::False )
	{ res.code = 400; res.end(); return; }
	const bool Record = Body["record"].b();
	std::vector<std::string> Tags;
	if( Body.has( "tags" ) )
	{
		if( !Record || Body["tags"].t() != crow::json::type::List || Body["tags"].size() > 8 )
		{ res.code = 400; res.end(); return; }
		for( size_t Index = 0; Index < Body["tags"].size(); ++Index )
		{
			if( Body["tags"][Index].t() != crow::json::type::String )
			{ res.code = 400; res.end(); return; }
			const std::string Tag = ApiTrim( Body["tags"][Index].s() );
			if( Tag.empty() || Tag.size() > 64 || !std::all_of( Tag.begin(), Tag.end(),
				[]( unsigned char Ch ) { return std::isalnum( Ch ) || Ch == '_' || Ch == '-' || Ch == ':' || Ch == '.'; } ) )
			{ res.code = 400; res.end(); return; }
			Tags.push_back( Tag );
		}
	}
	bool Exists = false, AlreadyRecording = false;
	{
		std::shared_lock<std::shared_mutex> Lock( m_GlobalContext->Mutex );
		const auto& Cameras = m_GlobalContext->GetCameraMap();
		const auto Camera = Cameras.find( cameraId );
		if( Camera != Cameras.end() )
		{
			Exists = true;
			AlreadyRecording = Camera->second.IsRecording;
		}
	}
	if( !Exists ) { res.code = 404; res.end(); return; }
	// Existing recordings cannot acquire start-trigger tags without ambiguity.
	if( Record && AlreadyRecording ) { res.code = 409; res.end(); return; }
	m_GlobalContext->MessageBus->SendToClient( nullptr,
		std::make_shared<CameraStateToggleRecordMessage>( cameraId, Record, std::move( Tags ) ) );
	crow::json::wvalue Result;
	Result["accepted"] = true;
	JsonReply( res, std::move( Result ), 202 );
}
