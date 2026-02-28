#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>

#include "crow/json.h"

class SQLiteDatabase;

// Shared setup configuration used by web wizard, CLI, and apply-config helper.
struct SetupConfig
{
	// Admin account
	std::string Username;
	std::string Password;

	// Server settings
	std::string Hostname;		// e.g. "localhost:8080"
	std::string TlsMode;		// "NoSecurity", "SelfSigned", "LetsEncrypt", "Manual"
	std::string TlsContact;	// email for Let's Encrypt
	std::string TlsCertPath;
	std::string TlsKeyPath;
	std::string WebRoot;
	std::string CachePath;

	// Startup
	std::string StartupMode;	// "Service", "Task", "Manual"

	bool LoadFromJson( const std::string& path )
	{
		std::ifstream file( path );
		if( !file )
		{
			std::cerr << "Cannot open config file: " << path << std::endl;
			return false;
		}

		std::string content( (std::istreambuf_iterator<char>(file)),
		                     std::istreambuf_iterator<char>() );

		auto json = crow::json::load( content );
		if( !json )
		{
			std::cerr << "Invalid JSON in config file: " << path << std::endl;
			return false;
		}

		if( json.has( "username" ) )    Username    = json["username"].s();
		if( json.has( "password" ) )    Password    = json["password"].s();
		if( json.has( "hostname" ) )    Hostname    = json["hostname"].s();
		if( json.has( "tls_mode" ) )    TlsMode     = json["tls_mode"].s();
		if( json.has( "tls_contact" ) ) TlsContact  = json["tls_contact"].s();
		if( json.has( "tls_cert" ) )    TlsCertPath = json["tls_cert"].s();
		if( json.has( "tls_key" ) )     TlsKeyPath  = json["tls_key"].s();
		if( json.has( "web_root" ) )    WebRoot     = json["web_root"].s();
		if( json.has( "cache_path" ) )  CachePath   = json["cache_path"].s();
		if( json.has( "startup_mode" ) ) StartupMode = json["startup_mode"].s();

		return true;
	}

	bool SaveToJson( const std::string& path ) const
	{
		crow::json::wvalue json;

		if( !Username.empty() )    json["username"]     = Username;
		if( !Password.empty() )    json["password"]     = Password;
		if( !Hostname.empty() )    json["hostname"]     = Hostname;
		if( !TlsMode.empty() )    json["tls_mode"]     = TlsMode;
		if( !TlsContact.empty() ) json["tls_contact"]  = TlsContact;
		if( !TlsCertPath.empty() ) json["tls_cert"]    = TlsCertPath;
		if( !TlsKeyPath.empty() ) json["tls_key"]      = TlsKeyPath;
		if( !WebRoot.empty() )    json["web_root"]      = WebRoot;
		if( !CachePath.empty() )  json["cache_path"]    = CachePath;
		if( !StartupMode.empty() ) json["startup_mode"] = StartupMode;

		std::ofstream file( path );
		if( !file )
		{
			std::cerr << "Cannot write config file: " << path << std::endl;
			return false;
		}

		file << json.dump();
		return true;
	}

	// Write non-privileged settings (DB settings, admin user) to the database
	bool ApplyToDatabase( const std::shared_ptr<SQLiteDatabase>& DB ) const;
};
