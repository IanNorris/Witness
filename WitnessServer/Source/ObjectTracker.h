#pragma once

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace Witness
{

struct TrackedBox
{
	float X, Y, W, H;  // normalized 0-1
	int ClassID;
	float Confidence;
	std::string ClassName;
};

struct TrackedObject
{
	unsigned int TrackingID;
	TrackedBox Box;
	int MissedFrames;
};

class ObjectTracker
{
public:

	ObjectTracker()
	: m_NextID( 1 )
	, m_MaxMissedFrames( 10 )
	, m_IoUThreshold( 0.3f )
	{}

	// Update tracker with new detections, returns boxes with assigned TrackingIDs
	std::vector<std::pair<TrackedBox, unsigned int>> Update( const std::vector<TrackedBox>& detections )
	{
		std::vector<std::pair<TrackedBox, unsigned int>> result;

		// Mark all existing objects as missed this frame
		for( auto& [id, obj] : m_Objects )
			obj.MissedFrames++;

		// Match new detections to existing tracked objects using IoU
		std::vector<bool> matched( detections.size(), false );

		for( auto& [id, obj] : m_Objects )
		{
			float bestIoU = 0.0f;
			int bestIdx = -1;

			for( size_t i = 0; i < detections.size(); i++ )
			{
				if( matched[i] )
					continue;

				float iou = ComputeIoU( obj.Box, detections[i] );
				if( iou > bestIoU )
				{
					bestIoU = iou;
					bestIdx = static_cast<int>( i );
				}
			}

			if( bestIdx >= 0 && bestIoU >= m_IoUThreshold )
			{
				matched[bestIdx] = true;
				obj.Box = detections[bestIdx];
				obj.MissedFrames = 0;
				result.push_back( { detections[bestIdx], id } );
			}
		}

		// Create new tracked objects for unmatched detections
		for( size_t i = 0; i < detections.size(); i++ )
		{
			if( !matched[i] )
			{
				unsigned int newID = m_NextID++;
				TrackedObject obj;
				obj.TrackingID = newID;
				obj.Box = detections[i];
				obj.MissedFrames = 0;
				m_Objects[newID] = obj;
				result.push_back( { detections[i], newID } );
			}
		}

		// Retire objects not seen for too many frames
		for( auto it = m_Objects.begin(); it != m_Objects.end(); )
		{
			if( it->second.MissedFrames > m_MaxMissedFrames )
				it = m_Objects.erase( it );
			else
				++it;
		}

		return result;
	}

	void Reset()
	{
		m_Objects.clear();
		m_NextID = 1;
	}

private:

	static float ComputeIoU( const TrackedBox& a, const TrackedBox& b )
	{
		float ax1 = a.X, ay1 = a.Y, ax2 = a.X + a.W, ay2 = a.Y + a.H;
		float bx1 = b.X, by1 = b.Y, bx2 = b.X + b.W, by2 = b.Y + b.H;

		float ix1 = std::max( ax1, bx1 );
		float iy1 = std::max( ay1, by1 );
		float ix2 = std::min( ax2, bx2 );
		float iy2 = std::min( ay2, by2 );

		if( ix2 <= ix1 || iy2 <= iy1 )
			return 0.0f;

		float intersection = ( ix2 - ix1 ) * ( iy2 - iy1 );
		float areaA = a.W * a.H;
		float areaB = b.W * b.H;
		float unionArea = areaA + areaB - intersection;

		return unionArea > 0.0f ? intersection / unionArea : 0.0f;
	}

	std::unordered_map<unsigned int, TrackedObject> m_Objects;
	unsigned int m_NextID;
	int m_MaxMissedFrames;
	float m_IoUThreshold;
};

} // namespace Witness
