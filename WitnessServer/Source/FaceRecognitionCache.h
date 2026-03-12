#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

class SQLiteDatabase;

// Known face identity with averaged embedding centroid
struct KnownFaceEntry
{
	int KnownFaceUID;
	std::string Name;
	std::vector<float> Centroid;       // Averaged L2-normalized embedding
	int EmbeddingCount;                // Number of verified embeddings contributing to centroid
};

// Result of matching a face embedding against known faces
struct FaceMatchResult
{
	int KnownFaceUID = 0;
	std::string Name;
	float Similarity = 0.0f;
	bool Matched = false;
};

// In-memory cache of known face embeddings for fast matching.
// Thread-safe: all public methods are mutex-protected.
class FaceRecognitionCache
{
public:

	FaceRecognitionCache();

	// Set minimum verified embedding count for matching (faces with fewer are skipped)
	void SetMinVerifiedCount( int count ) { m_MinVerifiedCount = std::max( 1, count ); }
	int GetMinVerifiedCount() const { return m_MinVerifiedCount; }

	// Load all verified embeddings from DB and compute centroids
	void LoadFromDatabase( const std::shared_ptr<SQLiteDatabase>& DB );

	// Match an embedding against all known faces
	// Returns best match if similarity >= threshold, otherwise Matched=false
	FaceMatchResult Match( const std::vector<float>& embedding, float threshold ) const;

	// Refresh cache (call after admin adds/removes/merges faces)
	void Refresh( const std::shared_ptr<SQLiteDatabase>& DB );

	// Get count of known faces in cache
	int GetKnownFaceCount() const;

	// Get all known face names (for diagnostics)
	std::vector<std::pair<int, std::string>> GetKnownFaces() const;

private:

	mutable std::mutex m_Mutex;
	std::vector<KnownFaceEntry> m_KnownFaces;
	int m_MinVerifiedCount = 2;
};
