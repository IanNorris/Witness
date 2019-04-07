#include "AzureVisionAnalysisEndpointFilter.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

struct TagToClassification
{
	unsigned int Classification;
	TCHAR* Name;
};

const static TagToClassification TagClassifications[] = {
	{ ClassificationResult::Motion_Animal | ClassificationResult::Motion_Animal_Cat, _T("cat") },
	{ ClassificationResult::Motion_Animal | ClassificationResult::Motion_Animal_Dog, _T("dog") },

	{ ClassificationResult::Motion_Animal, _T("animal") },
	{ ClassificationResult::Motion_Animal, _T("mammal") },

	{ ClassificationResult::Motion_Person, _T("boot") },
	{ ClassificationResult::Motion_Person, _T("shoe") },
	{ ClassificationResult::Motion_Person, _T("walking") },
	{ ClassificationResult::Motion_Person, _T("standing") },
	{ ClassificationResult::Motion_Person, _T("feet") },
	{ ClassificationResult::Motion_Person, _T("man") },
	{ ClassificationResult::Motion_Person, _T("woman") },
	{ ClassificationResult::Motion_Person, _T("person") },

	{ ClassificationResult::Motion_Vehicle, _T("car") },
	{ ClassificationResult::Motion_Vehicle, _T("truck") },
	{ ClassificationResult::Motion_Vehicle, _T("van") }
};

const static TCHAR* ListOfExclusions[] = {
	_T("outdoor"),
	_T("skiing"),
	_T("winter"),
	_T("snow"),
	_T("ground"),
	_T("brick"),
	_T("building"),
	_T("sidewalk"),
	_T("red"),
	_T("stone"),
	_T("building material"),
	_T("walkway"),
	_T("paving"),
	_T("curb"),
	_T("clothing"),
	_T("street"),
	_T("hockey"),
	_T("patio"),
	_T("cobblestone"),
	_T("driveway"),
	_T("tile"),
	_T("shadow"),
	_T("flag"),
	_T("white"),
	_T("grass"),
	_T("slope"),
	_T("covered"),
	_T("road"),
	_T("rubber"),
	_T("black"),
};

struct AzureVisionResultsCollect
{
	std::mutex Mutex;

	SharedClassificationTask TaskData;
	size_t WaitingForResults;
	bool Matched;
};

bool AzureVisionAnalysisEndpointFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	if (!IsAllowedToProcessFrame())
	{
		Continue( TaskData, false );
		return false;
	}

	json::value Request;
	QueryPairs Query;
	std::vector<unsigned char> Data;

	size_t Regions = TaskData->Result.ROI.size();

	if( Regions == 0 )
	{
		Continue( TaskData, false );
	}

	auto TaskCollect = make_shared<AzureVisionResultsCollect>();
	TaskCollect->TaskData = TaskData;
	TaskCollect->WaitingForResults = Regions;
	TaskCollect->Matched = false;

	auto TaskCallback = [TaskCollect,this]( bool Matched )
	{
		std::lock_guard<std::mutex> Lock(TaskCollect->Mutex);

		TaskCollect->Matched |= Matched;

		for( auto& ROI : TaskCollect->TaskData->Result.ROI )
		{
			TaskCollect->TaskData->Result.ClassificationSuperset |= ROI.Classification;
		}

		TaskCollect->WaitingForResults--;
		if( TaskCollect->WaitingForResults == 0 )
		{
			Continue( TaskCollect->TaskData, TaskCollect->Matched );
		}
	};

	for( int ROIIndex = 0; ROIIndex < Regions; ROIIndex++ )
	{
		auto& ROI = TaskData->Result.ROI[ROIIndex];

		//Ignore any object already classified
		if( ROI.Classification & (~ClassificationResult::Motion_Motion) )
		{
			TaskCallback( true );
			continue;
		}

		cv::Mat InputFrame = TaskData->Frame.GetOrDecodeFrame();

		float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

		const int TargetSize = 720;
		const int Quality = 60;

		cv::Mat ROIImage;

		float Downscale = 1.0f;
		ROIImage = cv::Mat( InputFrame, cv::Rect(
			(int)((float)ROI.Left * Downscale), 
			(int)((float)ROI.Top * Downscale), 
			(int)((float)ROI.Width * Downscale), 
			(int)((float)ROI.Height * Downscale)));

		cv::imencode( ".jpg", ROIImage, Data, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, Quality } );

		static int FrameIndex = 0;

		char Buffer[128];
		sprintf_s( Buffer, 128, "X:\\WitnessTemp\\%d.jpg", FrameIndex++ );

		ofstream Output( Buffer, ofstream::binary );

		Output.write( (const char*)&Data[0], Data.size() );

		Output.close();

		Query.push_back( QueryPair( _T("visualFeatures"), _T("Tags") ) ); // _T("Faces,Tags")

		const static double ConfidenceThreshold = 0.75;

		SendCommand( Analysis, Request, Query, Data ).then(
			[ROIIndex,TaskData,TaskCallback,this](web::http::http_response Response)
			{
				bool MatchMade = false;

				//tags -> array -> { name -> string, confidence -> double
				//wprintf(_T("%s\n"), Response.extract_string(true).get().c_str() );
				try
				{
					auto JsonBlob = Response.extract_json(false).get();

					if( JsonBlob.has_field(_T("tags") ) )
					{
						auto TagArray = JsonBlob[_T("tags")].as_array();
						for( auto& Tag : TagArray )
						{
							if( Tag.has_string_field(_T("name")) && Tag.has_double_field(_T("confidence")) )
							{
								string_t Name = Tag[_T("name")].as_string();
								double Confidence = Tag[_T("confidence")].as_double();

								if( Confidence >= ConfidenceThreshold )
								{
									bool IsExcluded = false;
									for( auto& Exclusion : ListOfExclusions )
									{
										if( Name.compare(Exclusion) == 0 )
										{
											IsExcluded = true;
											break;
										}
									}

									if( !IsExcluded )
									{
										auto& ROI = TaskData->Result.ROI[ROIIndex];

										bool Classified = false;
										for( auto& TagClass : TagClassifications )
										{
											if( Name.compare(TagClass.Name) == 0 )
											{
												//TODO: Need to task up the processing of the result,
												//as at the moment we're firing off multiple http async callbacks
												//and each of them fires the NextEvent.
												ROI.Classification |= TagClass.Classification;
												Classified = true;
												break;
											}
										}

										ROI.ClassificationConfidence = (float)Confidence;
										if( !Classified )
										{
											ROI.CustomLabel = std::string( Name.begin(), Name.end() );
										}

										MatchMade = true;
										wprintf(_T("%s -> %.3f\n"), Name.c_str(), (float)Confidence );
									}
								}
							}
						}
					}
				}
				catch (web::http::http_exception e)
				{
					auto ExceptionString = Hostname;
					std::cerr << "Error connecting to: " << std::string(ExceptionString.begin(), ExceptionString.end()) << ": " << e.what() << std::endl;
				}
				catch (std::exception e)
				{
					auto ExceptionString = Hostname;
					std::cerr << "Error connecting to: " << std::string(ExceptionString.begin(), ExceptionString.end()) << ": " << e.what() << std::endl;
				}

				if( !MatchMade )
				{
					wprintf(_T("Nothing of interest found\n") );
				}

				TaskCallback( MatchMade );
			}
		);
	}

	return false;
}

const string_t AzureVisionAnalysisEndpointFilter::CommandTypeToEndpoint(int CommandType)
{
	switch (CommandType)
	{
	case Analysis:
		return EndpointBase + _T("/analyze");

	default:
		assert(0);
		return _T("");
	}
}

const string_t AzureVisionAnalysisEndpointFilter::CommandTypeToMethod(int CommandType)
{
	switch (CommandType)
	{
	case Analysis:
		return _T("POST");

	default:
		assert(0);
		return _T("");
	}
}
