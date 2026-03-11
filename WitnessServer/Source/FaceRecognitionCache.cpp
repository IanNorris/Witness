#include "FaceRecognitionCache.h"
#include "SQLite.h"
#include <Log.h>
#include <cmath>
#include <algorithm>

FaceRecognitionCache::FaceRecognitionCache()
{
}

void FaceRecognitionCache::LoadFromDatabase( const std::shared_ptr<SQLiteDatabase>& DB )
{
	// Collect all verified embeddings grouped by KnownFaceUID
	struct RawEmbedding
	{
		int KnownFaceUID;
		std::vector<float> Embedding;
	};

	std::vector<RawEmbedding> rawEmbeddings;
	std::unordered_map<int, std::string> faceNames;

	// Load known face names
	{
		SQLiteDatabaseQueryInstance q( DB, "SelectAllKnownFaces" );
		q->Execute( [&]( const SQLiteDatabaseQuery& row )
		{
			int uid = row.GetColumnValueInt( 0 );
			const char* name = row.GetColumnValueText( 1 );
			faceNames[uid] = name ? name : "";
			return true;
		});
	}

	// Load verified embeddings
	{
		SQLiteDatabaseQueryInstance q( DB, "SelectVerifiedEmbeddings" );
		q->Execute( [&]( const SQLiteDatabaseQuery& row )
		{
			int knownFaceUID = row.GetColumnValueInt( 1 );
			const void* blobData = row.GetColumnValueBlob( 2 );
			int blobBytes = row.GetColumnValueBytes( 2 );
			int dimension = row.GetColumnValueInt( 3 );

			if( blobData && blobBytes > 0 && dimension > 0 )
			{
				int expectedBytes = dimension * (int)sizeof( float );
				if( blobBytes == expectedBytes )
				{
					RawEmbedding re;
					re.KnownFaceUID = knownFaceUID;
					re.Embedding.resize( dimension );
					memcpy( re.Embedding.data(), blobData, blobBytes );
					rawEmbeddings.push_back( std::move( re ) );
				}
			}
			return true;
		});
	}

	// Group by KnownFaceUID and compute centroids
	std::unordered_map<int, std::vector<const std::vector<float>*>> grouped;
	for( auto& re : rawEmbeddings )
	{
		grouped[re.KnownFaceUID].push_back( &re.Embedding );
	}

	std::vector<KnownFaceEntry> entries;
	for( auto& [uid, embeddings] : grouped )
	{
		if( embeddings.empty() ) continue;

		int dim = (int)embeddings[0]->size();
		KnownFaceEntry entry;
		entry.KnownFaceUID = uid;
		entry.Name = faceNames.count( uid ) ? faceNames[uid] : "Unknown";
		entry.EmbeddingCount = (int)embeddings.size();
		entry.Centroid.resize( dim, 0.0f );

		// Average all embeddings
		for( auto* emb : embeddings )
		{
			for( int i = 0; i < dim; i++ )
				entry.Centroid[i] += (*emb)[i];
		}
		for( int i = 0; i < dim; i++ )
			entry.Centroid[i] /= (float)embeddings.size();

		// L2 normalize the centroid
		float norm = 0.0f;
		for( float v : entry.Centroid )
			norm += v * v;
		norm = std::sqrt( norm );
		if( norm > 1e-6f )
		{
			for( float& v : entry.Centroid )
				v /= norm;
		}

		entries.push_back( std::move( entry ) );
	}

	// Swap into cache under lock
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_KnownFaces = std::move( entries );
	}

	LOG_INFO( "FaceRecognition: Cache loaded with %d known faces (%d total embeddings).",
		(int)m_KnownFaces.size(), (int)rawEmbeddings.size() );
}

FaceMatchResult FaceRecognitionCache::Match( const std::vector<float>& embedding, float threshold ) const
{
	std::lock_guard<std::mutex> lock( m_Mutex );

	FaceMatchResult best;
	best.Matched = false;
	best.Similarity = -1.0f;

	for( auto& known : m_KnownFaces )
	{
		if( known.Centroid.size() != embedding.size() )
			continue;

		// Cosine similarity (both are L2-normalized, so dot product suffices)
		float sim = 0.0f;
		for( size_t i = 0; i < embedding.size(); i++ )
			sim += embedding[i] * known.Centroid[i];

		if( sim > best.Similarity )
		{
			best.Similarity = sim;
			best.KnownFaceUID = known.KnownFaceUID;
			best.Name = known.Name;
		}
	}

	if( best.Similarity >= threshold )
		best.Matched = true;

	return best;
}

void FaceRecognitionCache::Refresh( const std::shared_ptr<SQLiteDatabase>& DB )
{
	LoadFromDatabase( DB );
}

int FaceRecognitionCache::GetKnownFaceCount() const
{
	std::lock_guard<std::mutex> lock( m_Mutex );
	return (int)m_KnownFaces.size();
}

std::vector<std::pair<int, std::string>> FaceRecognitionCache::GetKnownFaces() const
{
	std::lock_guard<std::mutex> lock( m_Mutex );
	std::vector<std::pair<int, std::string>> result;
	for( auto& kf : m_KnownFaces )
		result.push_back( { kf.KnownFaceUID, kf.Name } );
	return result;
}
