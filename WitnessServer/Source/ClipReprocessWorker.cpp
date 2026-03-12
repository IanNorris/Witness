#include "ClipReprocessWorker.h"
#include "TagHelpers.h"
#include "ObjectTracker.h"
#include "FaceRecognitionCache.h"

#include <Log.h>
#include <FaceEmbeddingModel.h>
#include <filesystem>
#include <set>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

ClipReprocessWorker::ClipReprocessWorker(
	const std::shared_ptr<MessageBus>& MessageBus,
	std::shared_ptr<SQLiteDatabase> Database,
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter,
	std::shared_ptr<Witness::Camera::FaceDetectionFilter> FaceFilter,
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbModel,
	std::shared_ptr<FaceRecognitionCache> FaceCache,
	double FaceRecThreshold,
	std::string CachePath,
	std::function<bool()> IsIdle
)
: WorkerBase( MessageBus )
, Database( std::move( Database ) )
, DetectionFilter( std::move( DetectionFilter ) )
, FaceFilter( std::move( FaceFilter ) )
, FaceEmbModel( std::move( FaceEmbModel ) )
, FaceCache( std::move( FaceCache ) )
, FaceRecThreshold( FaceRecThreshold )
, CachePath( std::move( CachePath ) )
, IsIdle( std::move( IsIdle ) )
{
}

struct ClipToReprocess
{
	int64_t ClipUID;
	int64_t Timestamp;
	int Camera;
	int RecordMode;
	std::string ExistingTags;
};

void ClipReprocessWorker::WorkerMain()
{
	UpdateLastTimedAction( "Idle" );

	// Yield to live detection when cameras have active motion
	if( !IsIdle() )
	{
		std::this_thread::sleep_for( std::chrono::seconds( 5 ) );
		return;
	}

	// Fetch a batch of clips needing reprocessing
	std::vector<ClipToReprocess> batch;

	{
		SQLiteDatabaseQueryInstance SelectClipForReprocess( Database, "SelectClipForReprocess" );
		SelectClipForReprocess->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		SelectClipForReprocess->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			ClipToReprocess clip;
			clip.ClipUID = query.GetColumnValueInt64( 0 );
			clip.Timestamp = query.GetColumnValueInt64( 1 );
			clip.Camera = query.GetColumnValueInt( 2 );
			clip.RecordMode = query.GetColumnValueInt( 6 );
			const char* tags = query.GetColumnValueText( 10 );
			if( tags )
				clip.ExistingTags = tags;
			batch.push_back( std::move( clip ) );
			return true;
		});
	}

	if( batch.empty() )
	{
		std::this_thread::sleep_for( std::chrono::seconds( 30 ) );
		return;
	}

	UpdateLastTimedAction( "Reprocessing clips" );

	for( auto& clip : batch )
	{
		// Yield if cameras become active mid-batch
		if( !IsIdle() )
			break;

		ProcessClip( clip.ClipUID, clip.Timestamp, clip.Camera, clip.RecordMode, clip.ExistingTags );

		// Sleep between clips to stay low-priority
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
	}
}

void ClipReprocessWorker::ProcessClip( int64_t clipUID, int64_t timestamp, int camera, int recordMode, const std::string& existingTags )
{
	// Build clip filename: {Camera}_{Auto|Manual}_{Timestamp}.mp4
	std::stringstream nameStream;
	nameStream << camera << "_" << ( recordMode == 1 ? "Auto" : "Manual" ) << "_" << timestamp << ".mp4";
	std::string clipPath = ( fs::path( CachePath ) / nameStream.str() ).string();

	if( !fs::exists( clipPath ) )
	{
		// File missing - mark as processed to skip it
		SQLiteDatabaseQueryInstance UpdateClipDetection( Database, "UpdateClipDetection" );
		UpdateClipDetection->Bind( "@ClipUID", clipUID );
		UpdateClipDetection->Bind( "@Tags", "" );
		UpdateClipDetection->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		UpdateClipDetection->Bind( "@Lighting", 0 );
		UpdateClipDetection->Execute( nullptr );

		// Clear any stale ClipTag rows for this missing clip
		{
			SQLiteDatabaseQueryInstance q( Database, "DeleteClipTags" );
			q->Bind( "@ClipUID", clipUID );
			q->Execute( nullptr );
		}

		LOG_DEBUG( "ClipReprocess: Clip %lld file not found, skipping: %s", (long long)clipUID, clipPath.c_str() );
		return;
	}

	// Open the clip with FFmpeg
	AVFormatContext* fmtCtx = nullptr;
	if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open clip %lld: %s", (long long)clipUID, clipPath.c_str() );
		return;
	}

	if( avformat_find_stream_info( fmtCtx, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to find stream info for clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Find video stream
	int videoIdx = -1;
	for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
	{
		if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			videoIdx = i;
			break;
		}
	}

	if( videoIdx < 0 )
	{
		LOG_ERROR( "ClipReprocess: No video stream in clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Open decoder
	auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
	auto codec = avcodec_find_decoder( codecpar->codec_id );
	if( !codec )
	{
		LOG_ERROR( "ClipReprocess: No decoder for clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	auto codecCtx = avcodec_alloc_context3( codec );
	avcodec_parameters_to_context( codecCtx, codecpar );
	if( avcodec_open2( codecCtx, codec, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open decoder for clip %lld", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Set up SwsContext for pixel format conversion to BGR24
	SwsContext* swsCtx = sws_getContext(
		codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
		codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
		SWS_BILINEAR, nullptr, nullptr, nullptr
	);

	if( !swsCtx )
	{
		LOG_ERROR( "ClipReprocess: Failed to create SwsContext for clip %lld", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Determine clip duration for sampling every 2 seconds
	AVStream* videoStream = fmtCtx->streams[videoIdx];
	double durationSec = 0.0;
	if( fmtCtx->duration > 0 )
		durationSec = (double)fmtCtx->duration / AV_TIME_BASE;
	else if( videoStream->duration > 0 && videoStream->time_base.den > 0 )
		durationSec = (double)videoStream->duration * av_q2d( videoStream->time_base );

	std::set<std::string> detectedTags;
	Witness::Camera::LightingCondition lighting = Witness::Camera::LightingCondition::Unknown;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVFrame* bgrFrame = av_frame_alloc();

	int frameWidth = codecCtx->width;
	int frameHeight = codecCtx->height;

	// Clear old detection overlay data for this clip's time range before re-generating
	{
		double fromTs = static_cast<double>( timestamp );
		double toTs = static_cast<double>( timestamp ) + durationSec + 1.0;
		SQLiteDatabaseQueryInstance delDet( Database, "DeleteDetectionFramesInRange" );
		delDet->Bind( "@CameraID", camera );
		delDet->Bind( "@TimestampFrom", fromTs );
		delDet->Bind( "@TimestampTo", toTs );
		delDet->Execute( nullptr );
	}

	// Allocate BGR frame buffer
	int bgrBufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );
	std::vector<uint8_t> bgrBuffer( bgrBufSize );
	av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, bgrBuffer.data(),
		AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );

	// Sample frames at 2-second intervals
	// First frame is used as baseline - objects present before motion are filtered out
	// Uses spatial IoU matching: a detection is filtered only if same class AND >50% overlap
	double sampleInterval = 2.0;
	double currentTime = 0.0;
	std::vector<Witness::Camera::DetectionResult> baselineDetections;
	bool baselineCaptured = false;
	Witness::ObjectTracker clipTracker;

	while( currentTime <= durationSec || currentTime == 0.0 )
	{
		// Seek to target time
		int64_t seekTarget = (int64_t)( currentTime * AV_TIME_BASE );
		if( currentTime > 0.0 )
		{
			av_seek_frame( fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD );
			avcodec_flush_buffers( codecCtx );
		}

		// Decode one frame
		bool gotFrame = false;
		while( av_read_frame( fmtCtx, pkt ) >= 0 )
		{
			if( pkt->stream_index == videoIdx )
			{
				avcodec_send_packet( codecCtx, pkt );
				if( avcodec_receive_frame( codecCtx, frame ) == 0 )
				{
					// Convert to BGR24 cv::Mat
					sws_scale( swsCtx, frame->data, frame->linesize, 0, frameHeight,
						bgrFrame->data, bgrFrame->linesize );

					cv::Mat mat( frameHeight, frameWidth, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );

					// Classify lighting on first frame
					if( !baselineCaptured )
						lighting = Witness::Camera::ClassifyLighting( mat );

					// Apply CLAHE for night/IR frames to improve detection
					cv::Mat detectionMat = ( lighting == Witness::Camera::LightingCondition::Night )
						? Witness::Camera::ApplyCLAHE( mat )
						: mat;

					// Run ONNX detection
					auto detections = DetectionFilter->DetectFrame( detectionMat );

					if( !baselineCaptured )
					{
						// First frame: capture all detections as baseline (with bounding boxes)
						baselineDetections = detections;
						baselineCaptured = true;
					}

					// Classify each detection as baseline or new
					struct ClassifiedDetection
					{
						Witness::Camera::DetectionResult det;
						bool isBaseline;
					};
					std::vector<ClassifiedDetection> classified;

					for( auto& det : detections )
					{
						bool matchesBaseline = false;
						for( auto& base : baselineDetections )
						{
							if( det.ClassId != base.ClassId )
								continue;
							float ix1 = std::max( det.X1, base.X1 );
							float iy1 = std::max( det.Y1, base.Y1 );
							float ix2 = std::min( det.X2, base.X2 );
							float iy2 = std::min( det.Y2, base.Y2 );
							float iw = std::max( 0.0f, ix2 - ix1 );
							float ih = std::max( 0.0f, iy2 - iy1 );
							float intersection = iw * ih;
							float areaA = ( det.X2 - det.X1 ) * ( det.Y2 - det.Y1 );
							float areaB = ( base.X2 - base.X1 ) * ( base.Y2 - base.Y1 );
							float unionArea = areaA + areaB - intersection;
							float iou = unionArea > 0.0f ? intersection / unionArea : 0.0f;
							if( iou > 0.5f )
							{
								matchesBaseline = true;
								break;
							}
						}
						if( !matchesBaseline )
							detectedTags.insert( det.ClassName );
						classified.push_back( { det, matchesBaseline } );
					}

					// Store all detection boxes (baseline + new) for overlay playback
					if( !classified.empty() )
					{
						// Run face detection on person crops if face filter available
						std::vector<Witness::Camera::FaceDetectionResult> faceResults;
						if( FaceFilter && FaceFilter->IsModelLoaded() )
						{
							// Build ROIs from person detections for face detection
							std::vector<Witness::Camera::ClassificationResult::RegionOfInterest> personROIs;
							for( auto& c : classified )
							{
								if( c.det.ClassName == "person" )
								{
									Witness::Camera::ClassificationResult::RegionOfInterest roi;
									roi.Left = static_cast<unsigned int>( c.det.X1 * frameWidth );
									roi.Top = static_cast<unsigned int>( c.det.Y1 * frameHeight );
									roi.Width = static_cast<unsigned int>( ( c.det.X2 - c.det.X1 ) * frameWidth );
									roi.Height = static_cast<unsigned int>( ( c.det.Y2 - c.det.Y1 ) * frameHeight );
									roi.Classification = Witness::Camera::ClassificationResult::Motion_Person;
									personROIs.push_back( roi );
								}
							}
							if( !personROIs.empty() )
							{
								faceResults = FaceFilter->DetectFacesInPersonCrops( mat, personROIs );
								LOG_DEBUG( "ClipReprocess: Face detection on %zu person crops -> %zu faces (clip %lld)",
									personROIs.size(), faceResults.size(), (long long)clipUID );
							}
						}

						// Run tracker for stable IDs
						std::vector<Witness::TrackedBox> trackInputs;
						for( auto& c : classified )
						{
							Witness::TrackedBox tb;
							tb.X = c.det.X1; tb.Y = c.det.Y1;
							tb.W = c.det.X2 - c.det.X1; tb.H = c.det.Y2 - c.det.Y1;
							tb.ClassID = c.det.ClassId;
							tb.Confidence = c.det.Confidence;
							tb.ClassName = c.det.ClassName;
							trackInputs.push_back( std::move( tb ) );
						}
						// Add face detections to tracker
						for( auto& face : faceResults )
						{
							Witness::TrackedBox tb;
							tb.X = face.X1 / frameWidth; tb.Y = face.Y1 / frameHeight;
							tb.W = ( face.X2 - face.X1 ) / frameWidth;
							tb.H = ( face.Y2 - face.Y1 ) / frameHeight;
							tb.ClassID = 100;  // FACE_CLASS_ID
							tb.Confidence = face.Confidence;
							tb.ClassName = "face";
							trackInputs.push_back( std::move( tb ) );
						}
						auto tracked = clipTracker.Update( trackInputs );

						double frameTimestamp = static_cast<double>( timestamp ) + currentTime;
						int64_t frameUID = 0;
						{
							SQLiteDatabaseQueryInstance q( Database, "InsertDetectionFrame" );
							q->Bind( "@CameraID", camera );
							q->Bind( "@Timestamp", frameTimestamp );
							q->Bind( "@FrameWidth", frameWidth );
							q->Bind( "@FrameHeight", frameHeight );
							q->Execute( nullptr );
							frameUID = q->GetLastInsertionId();
						}

						// Prepare crop directories
						auto cropsDir = fs::path( CachePath ) / "crops" / std::to_string( camera );
						auto facesDir = fs::path( CachePath ) / "faces" / std::to_string( camera );
						bool cropsDirCreated = false;
						bool facesDirCreated = false;

						for( size_t i = 0; i < tracked.size(); i++ )
						{
							auto& [box, trackID] = tracked[i];
							bool isBase = ( i < classified.size() ) ? classified[i].isBaseline : false;

							// Crop detection region
							std::string cropPath;
							int cropX = static_cast<int>( box.X * frameWidth );
							int cropY = static_cast<int>( box.Y * frameHeight );
							int cropW = static_cast<int>( box.W * frameWidth );
							int cropH = static_cast<int>( box.H * frameHeight );
							cropX = std::max( 0, cropX );
							cropY = std::max( 0, cropY );
							cropW = std::min( cropW, mat.cols - cropX );
							cropH = std::min( cropH, mat.rows - cropY );

							if( cropW > 10 && cropH > 10 && !box.ClassName.empty() )
							{
								if( !cropsDirCreated )
								{
									fs::create_directories( cropsDir );
									cropsDirCreated = true;
								}

								cv::Rect cropRect( cropX, cropY, cropW, cropH );
								cv::Mat cropped = mat( cropRect ).clone();
								int maxDim = std::max( cropped.cols, cropped.rows );
								if( maxDim > 224 )
								{
									double scale = 224.0 / maxDim;
									cv::resize( cropped, cropped, cv::Size(), scale, scale );
								}

								std::ostringstream filename;
								filename << std::fixed << std::setprecision( 3 ) << frameTimestamp
									<< "_" << trackID << ".jpg";
								auto filePath = cropsDir / filename.str();

								std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 80 };
								cv::imwrite( filePath.string(), cropped, params );
								cropPath = filePath.string();
							}

							SQLiteDatabaseQueryInstance q( Database, "InsertDetectionBox" );
							q->Bind( "@FrameUID", frameUID );
							q->Bind( "@TrackingID", static_cast<int>( trackID ) );
							q->Bind( "@ClassID", box.ClassID );
							q->Bind( "@ClassName", box.ClassName.c_str() );
							q->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
							q->Bind( "@X", static_cast<double>( box.X ) );
							q->Bind( "@Y", static_cast<double>( box.Y ) );
							q->Bind( "@W", static_cast<double>( box.W ) );
							q->Bind( "@H", static_cast<double>( box.H ) );
							q->Bind( "@IsBaseline", isBase ? 1 : 0 );
							if( !cropPath.empty() )
								q->Bind( "@CropPath", cropPath.c_str() );
							q->Execute( nullptr );

							// Save 112x112 face crop + landmarks
							if( box.ClassName == "face" && cropW > 10 && cropH > 10 )
							{
								if( !facesDirCreated )
								{
									fs::create_directories( facesDir );
									facesDirCreated = true;
								}

								cv::Rect faceRect( cropX, cropY, cropW, cropH );
								cv::Mat faceCrop;
								cv::resize( mat( faceRect ), faceCrop, cv::Size( 112, 112 ) );

								std::ostringstream faceFilename;
								faceFilename << std::fixed << std::setprecision( 3 ) << frameTimestamp
									<< "_" << trackID << ".jpg";
								auto facePath = facesDir / faceFilename.str();

								std::vector<int> faceParams = { cv::IMWRITE_JPEG_QUALITY, 85 };
								cv::imwrite( facePath.string(), faceCrop, faceParams );

								// Find matching face result for landmarks
								float lmX[5] = {}, lmY[5] = {};
								for( auto& fr : faceResults )
								{
									float fx = fr.X1 / frameWidth;
									float fy = fr.Y1 / frameHeight;
									if( std::abs( fx - box.X ) < 0.01f && std::abs( fy - box.Y ) < 0.01f )
									{
										// Normalize landmarks to crop-relative 0-1
										for( int lm = 0; lm < 5; lm++ )
										{
											lmX[lm] = ( fr.LandmarkX[lm] / frameWidth - box.X ) / box.W;
											lmY[lm] = ( fr.LandmarkY[lm] / frameHeight - box.Y ) / box.H;
										}
										break;
									}
								}

								SQLiteDatabaseQueryInstance fq( Database, "InsertFaceCrop" );
								fq->Bind( "@CameraID", camera );
								fq->Bind( "@Timestamp", frameTimestamp );
								fq->Bind( "@FrameUID", frameUID );
								fq->Bind( "@TrackingID", static_cast<int>( trackID ) );
								fq->Bind( "@FilePath", facePath.string().c_str() );
								fq->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
								fq->Bind( "@Landmark0X", static_cast<double>( lmX[0] ) );
								fq->Bind( "@Landmark0Y", static_cast<double>( lmY[0] ) );
								fq->Bind( "@Landmark1X", static_cast<double>( lmX[1] ) );
								fq->Bind( "@Landmark1Y", static_cast<double>( lmY[1] ) );
								fq->Bind( "@Landmark2X", static_cast<double>( lmX[2] ) );
								fq->Bind( "@Landmark2Y", static_cast<double>( lmY[2] ) );
								fq->Bind( "@Landmark3X", static_cast<double>( lmX[3] ) );
								fq->Bind( "@Landmark3Y", static_cast<double>( lmY[3] ) );
								fq->Bind( "@Landmark4X", static_cast<double>( lmX[4] ) );
								fq->Bind( "@Landmark4Y", static_cast<double>( lmY[4] ) );
								fq->Execute( nullptr );
								int64_t cropUID = fq->GetLastInsertionId();

								// Generate face embedding if model is available
								if( FaceEmbModel && FaceEmbModel->IsModelLoaded() && box.Confidence >= 0.8f )
								{
									bool hasLandmarks = false;
									for( int lm = 0; lm < 5; lm++ )
									{
										if( lmX[lm] != 0.0f || lmY[lm] != 0.0f ) { hasLandmarks = true; break; }
									}

									if( hasLandmarks )
									{
										// Convert crop-relative 0-1 landmarks to 112px crop pixels for geometry check
										float cropLmX[5], cropLmY[5];
										for( int lm = 0; lm < 5; lm++ )
										{
											cropLmX[lm] = lmX[lm] * 112.0f;
											cropLmY[lm] = lmY[lm] * 112.0f;
										}

										// Orientation validation (same thresholds as CameraWorker)
										float eyeAvgY = ( cropLmY[0] + cropLmY[1] ) * 0.5f;
										float mouthAvgY = ( cropLmY[3] + cropLmY[4] ) * 0.5f;
										float interEyeDist = std::abs( cropLmX[1] - cropLmX[0] );
										float eyeMidX = ( cropLmX[0] + cropLmX[1] ) * 0.5f;
										float noseOffsetX = std::abs( cropLmX[2] - eyeMidX );
										float eyeHeightDiff = std::abs( cropLmY[0] - cropLmY[1] );

										bool validGeometry = eyeAvgY < cropLmY[2] && cropLmY[2] < mouthAvgY
											&& interEyeDist > 12.0f
											&& noseOffsetX < 30.0f
											&& eyeHeightDiff < 25.0f;

										if( validGeometry )
										{
											cv::Mat alignedFace = Witness::Camera::FaceEmbeddingModel::AlignFace( faceCrop, cropLmX, cropLmY );
											auto embedding = FaceEmbModel->GetEmbedding( alignedFace );

											if( !embedding.empty() )
											{
												double matchConf = 0.0;
												int matchedKnownFaceUID = 0;

												if( FaceCache )
												{
													auto match = FaceCache->Match( embedding, (float)FaceRecThreshold );
													if( match.Matched )
													{
														matchConf = match.Similarity;
														matchedKnownFaceUID = match.KnownFaceUID;
														LOG_INFO( "ClipReprocess: Face match '%s' (%.3f) crop %lld",
															match.Name.c_str(), matchConf, (long long)cropUID );
													}
													else
													{
														LOG_DEBUG( "ClipReprocess: No match for crop %lld (best=%.3f '%s', threshold=%.3f)",
															(long long)cropUID, match.Similarity,
															match.Name.c_str(), FaceRecThreshold );
													}
												}

												SQLiteDatabaseQueryInstance embQ( Database, "InsertFaceEmbedding" );
												embQ->Bind( "@FaceCropUID", static_cast<int>( cropUID ) );
												if( matchedKnownFaceUID > 0 )
													embQ->Bind( "@KnownFaceUID", matchedKnownFaceUID );
												else
													embQ->BindNull( "@KnownFaceUID" );
												embQ->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
												embQ->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
												embQ->Bind( "@MatchConfidence", matchConf );
												embQ->Bind( "@Verified", 0 );
												embQ->Bind( "@CreatedAt", frameTimestamp );
												embQ->Execute( nullptr );

												LOG_DEBUG( "ClipReprocess: Stored embedding for crop %lld (conf=%.2f, match=%d)",
													(long long)cropUID, box.Confidence, matchedKnownFaceUID );
											}
										}
										else
										{
											LOG_DEBUG( "ClipReprocess: Orientation rejected crop %lld (interEye=%.1f nose=%.1f eyeDiff=%.1f)",
												(long long)cropUID, interEyeDist, noseOffsetX, eyeHeightDiff );
										}
									}
									else
									{
										LOG_DEBUG( "ClipReprocess: No landmarks for crop %lld", (long long)cropUID );
									}
								}

								detectedTags.insert( "face" );
							}
						}
					}

					gotFrame = true;
					av_packet_unref( pkt );
					break;
				}
			}
			av_packet_unref( pkt );
		}

		if( !gotFrame )
			break;

		currentTime += sampleInterval;
	}

	// Cleanup FFmpeg resources
	av_frame_free( &bgrFrame );
	av_frame_free( &frame );
	av_packet_free( &pkt );
	sws_freeContext( swsCtx );
	avcodec_free_context( &codecCtx );
	avformat_close_input( &fmtCtx );

	// Build combined tag string (merge with existing tags)
	std::set<std::string> allTags;
	if( !existingTags.empty() )
	{
		std::istringstream iss( existingTags );
		std::string tag;
		while( std::getline( iss, tag, ';' ) )
		{
			size_t start = tag.find_first_not_of( ' ' );
			size_t end = tag.find_last_not_of( ' ' );
			if( start != std::string::npos )
				allTags.insert( tag.substr( start, end - start + 1 ) );
		}
	}
	allTags.insert( detectedTags.begin(), detectedTags.end() );

	std::string tagString;
	for( auto& t : allTags )
	{
		if( !tagString.empty() )
			tagString += ";";
		tagString += t;
	}

	// Update clip in database
	{
		SQLiteDatabaseQueryInstance UpdateClipDetection( Database, "UpdateClipDetection" );
		UpdateClipDetection->Bind( "@ClipUID", clipUID );
		UpdateClipDetection->Bind( "@Tags", tagString.c_str() );
		UpdateClipDetection->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		UpdateClipDetection->Bind( "@Lighting", (int)lighting );
		UpdateClipDetection->Execute( nullptr );
	}

	// Sync ClipTag junction table
	TagHelpers::SyncClipTags( Database, clipUID, tagString );

	LOG_INFO( "Reprocessed clip %lld: %s", (long long)clipUID, tagString.c_str() );
}

void ClipReprocessWorker::BackfillLighting()
{
	struct ClipInfo { int64_t ClipUID; int64_t Timestamp; int Camera; int RecordMode; };
	std::vector<ClipInfo> batch;

	{
		SQLiteDatabaseQueryInstance q( Database, "SelectClipsNeedingLighting" );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			ClipInfo c;
			c.ClipUID = query.GetColumnValueInt64( 0 );
			c.Timestamp = query.GetColumnValueInt64( 1 );
			c.Camera = query.GetColumnValueInt( 2 );
			c.RecordMode = query.GetColumnValueInt( 3 );
			batch.push_back( c );
			return true;
		});
	}

	if( batch.empty() )
	{
		LightingBackfillComplete = true;
		LOG_INFO( "Lighting backfill complete" );
		return;
	}

	UpdateLastTimedAction( "Backfilling lighting" );
	int classified = 0;

	for( auto& clip : batch )
	{
		if( !IsIdle() )
			break;

		std::stringstream nameStream;
		nameStream << clip.Camera << "_" << ( clip.RecordMode == 1 ? "Auto" : "Manual" ) << "_" << clip.Timestamp << ".mp4";
		std::string clipPath = ( fs::path( CachePath ) / nameStream.str() ).string();

		int lighting = 0; // Unknown

		if( fs::exists( clipPath ) )
		{
			// Open clip, decode first frame, classify lighting
			AVFormatContext* fmtCtx = nullptr;
			if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) >= 0 )
			{
				if( avformat_find_stream_info( fmtCtx, nullptr ) >= 0 )
				{
					int videoIdx = -1;
					for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
					{
						if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
						{ videoIdx = i; break; }
					}

					if( videoIdx >= 0 )
					{
						auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
						auto codec = avcodec_find_decoder( codecpar->codec_id );
						if( codec )
						{
							auto codecCtx = avcodec_alloc_context3( codec );
							avcodec_parameters_to_context( codecCtx, codecpar );
							if( avcodec_open2( codecCtx, codec, nullptr ) >= 0 )
							{
								SwsContext* swsCtx = sws_getContext(
									codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
									codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
									SWS_BILINEAR, nullptr, nullptr, nullptr );

								if( swsCtx )
								{
									AVPacket* pkt = av_packet_alloc();
									AVFrame* frame = av_frame_alloc();
									AVFrame* bgrFrame = av_frame_alloc();
									int w = codecCtx->width, h = codecCtx->height;
									int bufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, w, h, 1 );
									std::vector<uint8_t> buf( bufSize );
									av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, buf.data(), AV_PIX_FMT_BGR24, w, h, 1 );

									// Decode first video frame
									while( av_read_frame( fmtCtx, pkt ) >= 0 )
									{
										if( pkt->stream_index == videoIdx )
										{
											avcodec_send_packet( codecCtx, pkt );
											if( avcodec_receive_frame( codecCtx, frame ) == 0 )
											{
												sws_scale( swsCtx, frame->data, frame->linesize, 0, h,
													bgrFrame->data, bgrFrame->linesize );
												cv::Mat mat( h, w, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );
												lighting = (int)Witness::Camera::ClassifyLighting( mat );
												av_packet_unref( pkt );
												break;
											}
										}
										av_packet_unref( pkt );
									}

									av_frame_free( &bgrFrame );
									av_frame_free( &frame );
									av_packet_free( &pkt );
									sws_freeContext( swsCtx );
								}
							}
							avcodec_free_context( &codecCtx );
						}
					}
				}
				avformat_close_input( &fmtCtx );
			}
		}

		// Update just the lighting column
		{
			SQLiteDatabaseQueryInstance q( Database, "UpdateClipLighting" );
			q->Bind( "@ClipUID", clip.ClipUID );
			q->Bind( "@Lighting", lighting );
			q->Execute( nullptr );
		}
		classified++;
	}

	LOG_INFO( "Lighting backfill: classified %d clips this batch", classified );
}
