#pragma once

// Dashboard persistence on a raw sqlite3 handle (testable against a temporary
// database). Visibility rules:
//   personal (Scope 0): owner only (admins included in "only")
//   shared   (Scope 1): admins, plus principals in DashboardShare
//                       (everyone / a user / a group the user belongs to)
// Editing: owner for personal, administrators for shared.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace Dashboard
{
	enum class Scope : int { Personal = 0, Shared = 1 };
	enum class PrincipalType : int { Everyone = 0, User = 1, Group = 2 };

	struct Row
	{
		int64_t Id = 0;
		int OwnerUserUID = 0;
		Scope DashboardScope = Scope::Personal;
		std::string Name;
		std::string Document;
		int Revision = 0;
		int64_t CreatedAt = 0;
		int64_t UpdatedAt = 0;
		int UpdatedByUserUID = 0;
	};

	struct Principal
	{
		PrincipalType Type = PrincipalType::Everyone;
		int Id = 0;
		bool operator==( const Principal& other ) const { return Type == other.Type && Id == other.Id; }
	};

	struct RevisionInfo
	{
		int Revision = 0;
		int64_t UpdatedAt = 0;
		int UpdatedByUserUID = 0;
		std::string Name;
	};

	enum class UpdateResult { Ok, NotFound, Conflict };

	constexpr int KeptRevisions = 10;

	void EnsureSchema( sqlite3* db );

	bool CanView( sqlite3* db, const Row& row, int userUID, bool admin );
	bool CanEdit( const Row& row, int userUID, bool admin );

	std::vector<Row> ListVisible( sqlite3* db, int userUID, bool admin );
	std::optional<Row> Get( sqlite3* db, int64_t id );

	int CountOwnedPersonal( sqlite3* db, int userUID );
	int64_t Create( sqlite3* db, int ownerUserUID, Scope scope, const std::string& name, const std::string& document, int64_t now );
	UpdateResult Update( sqlite3* db, int64_t id, int baseRevision, const std::string& name, const std::string& document,
		int userUID, int64_t now, int* newRevision );
	bool Delete( sqlite3* db, int64_t id );

	std::vector<Principal> GetShares( sqlite3* db, int64_t id );
	void SetShares( sqlite3* db, int64_t id, const std::vector<Principal>& principals );

	std::vector<RevisionInfo> ListRevisions( sqlite3* db, int64_t id );
	std::optional<std::string> GetRevisionDocument( sqlite3* db, int64_t id, int revision );
}

// Called from Database initialisation beside EnsureCapturedClipSchema.
inline void EnsureDashboardSchema( sqlite3* db ) { Dashboard::EnsureSchema( db ); }
