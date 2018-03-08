/*
Copyright (c) 2010-2016, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rtabmap/core/OdometryOkvis.h"
#include "rtabmap/core/OdometryInfo.h"
#include "rtabmap/core/util3d_transforms.h"
#include "rtabmap/utilite/ULogger.h"
#include "rtabmap/utilite/UTimer.h"
#include "rtabmap/utilite/UStl.h"
#include "rtabmap/utilite/UThread.h"

#ifdef RTABMAP_OKVIS
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <memory>
#include <functional>
#include <atomic>

#include <Eigen/Core>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#include <opencv2/opencv.hpp>
#pragma GCC diagnostic pop
#include <okvis/VioParametersReader.hpp>
#include <okvis/ThreadedKFVio.hpp>
#include <okvis/cameras/PinholeCamera.hpp>
#include <okvis/cameras/NoDistortion.hpp>
#include <okvis/cameras/RadialTangentialDistortion.hpp>
#include <okvis/cameras/EquidistantDistortion.hpp>
#include <okvis/cameras/RadialTangentialDistortion8.hpp>
#include <boost/filesystem.hpp>
#endif

namespace rtabmap {

#ifdef RTABMAP_OKVIS
class OkvisCallbackHandler
{
public:
	OkvisCallbackHandler()
	{
	}

	Transform getLastTransform()
	{
		Transform tf;
		mutex_.lock();
		tf = transform_;
		mutex_.unlock();

		return tf;
	}

	std::map<int, cv::Point3f> getLastLandmarks()
	{
		std::map<int, cv::Point3f> landmarks;
		mutexLandmarks_.lock();
		landmarks = landmarks_;
		mutexLandmarks_.unlock();
		return landmarks;
	}

public:
	void fullStateCallback(
	      const okvis::Time & t, const okvis::kinematics::Transformation & T_WS,
	      const Eigen::Matrix<double, 9, 1> & /*speedAndBiases*/,
	      const Eigen::Matrix<double, 3, 1> & /*omega_S*/)
	{
		UDEBUG("");
		Transform tf = Transform::fromEigen4d(T_WS.T());

		mutex_.lock();
		transform_ = tf;
		mutex_.unlock();
	}

	void landmarksCallback(const okvis::Time & t,
			const okvis::MapPointVector & landmarksVector,
			const okvis::MapPointVector & /*transferredLandmarks*/)
	{
		bool notify = true;
		mutexLandmarks_.lock();
		if(landmarks_.size())
		{
			notify = false;
		}
		landmarks_.clear();
		for(unsigned int i=0; i<landmarksVector.size(); ++i)
		{
			landmarks_.insert(std::make_pair((int)landmarksVector[i].id, cv::Point3f(landmarksVector[i].point[0], landmarksVector[i].point[1], landmarksVector[i].point[2])));
		}
		mutexLandmarks_.unlock();
		if(notify)
		{
			semLandmarks_.release();
		}
	}



private:
	Transform transform_;
	std::map<int, cv::Point3f> landmarks_;
	UMutex mutex_;
	UMutex mutexLandmarks_;
	USemaphore semTf_;
	USemaphore semLandmarks_;
};
#endif

OdometryOkvis::OdometryOkvis(const ParametersMap & parameters) :
	Odometry(parameters),
#ifdef RTABMAP_OKVIS
	okvisCallbackHandler_(new OkvisCallbackHandler),
#else
	okvisCallbackHandler_(0),
#endif
	okvisEstimator_(0),
	okvisParameters_(parameters),
	imagesProcessed_(0)
{
#ifdef RTABMAP_OKVIS
	Parameters::parse(parameters, Parameters::kOdomOKVISConfigPath(), configFilename_);
	if(configFilename_.empty())
	{
		UERROR("OKVIS config file is empty (%s)!", Parameters::kOdomOKVISConfigPath().c_str());
	}
#endif
}

OdometryOkvis::~OdometryOkvis()
{
#ifdef RTABMAP_OKVIS
	if(okvisEstimator_)
	{
		delete okvisEstimator_;
	}
	delete okvisCallbackHandler_;
#endif
}

void OdometryOkvis::reset(const Transform & initialPose)
{
	Odometry::reset(initialPose);
#ifdef RTABMAP_OKVIS
	if(okvisEstimator_)
	{
		delete okvisEstimator_;
		okvisEstimator_ = 0;
	}
	lastImu_ = IMU();

	delete okvisCallbackHandler_;
	okvisCallbackHandler_ = new OkvisCallbackHandler();
#endif
	imagesProcessed_ = 0;
}

// return not null transform if odometry is correctly computed
Transform OdometryOkvis::computeTransform(
		SensorData & data,
		const Transform & guess,
		OdometryInfo * info)
{
	UDEBUG("");
	Transform t;
#ifdef RTABMAP_OKVIS
	UTimer timer;

	okvis::Time timeOkvis = okvis::Time(data.stamp());

	bool imuUpdated = false;
	if(!data.imu().empty())
	{
		UDEBUG("IMU update stamp=%f acc=%f %f %f gyr=%f %f %f", data.stamp(),
				data.imu().linearAcceleration()[0],
				data.imu().linearAcceleration()[1],
				data.imu().linearAcceleration()[2],
				data.imu().angularVelocity()[0],
				data.imu().angularVelocity()[1],
				data.imu().angularVelocity()[2]);
		if(okvisEstimator_ != 0)
		{
			imuUpdated = okvisEstimator_->addImuMeasurement(timeOkvis, data.imu().linearAcceleration(), data.imu().angularVelocity());
		}
		else
		{
			UWARN("Ignoring IMU, waiting for an image to initialize...");
			lastImu_ = data.imu();
		}
	}

	bool imageUpdated = false;
	if(!data.imageRaw().empty())
	{
		UDEBUG("Image update stamp=%f", data.stamp());
		std::vector<cv::Mat> images;
		std::vector<CameraModel> models;
		if(data.stereoCameraModel().isValidForProjection())
		{
			images.push_back(data.imageRaw());
			images.push_back(data.rightRaw());
			CameraModel mleft = data.stereoCameraModel().left();
			// should be transform between IMU and camera
			mleft.setLocalTransform(lastImu_.localTransform().inverse()*mleft.localTransform());
			models.push_back(mleft);
			CameraModel mright = data.stereoCameraModel().right();

			// To support not rectified images
			if(!imagesAlreadyRectified())
			{
				cv::Mat R = data.stereoCameraModel().R();
				cv::Mat T = data.stereoCameraModel().T();
				UASSERT(R.cols==3 && R.rows == 3);
				UASSERT(T.cols==1 && T.rows == 3);
				Transform extrinsics(R.at<double>(0,0), R.at<double>(0,1), R.at<double>(0,2), T.at<double>(0,0),
									R.at<double>(1,0), R.at<double>(1,1), R.at<double>(1,2), T.at<double>(1,0),
									R.at<double>(2,0), R.at<double>(2,1), R.at<double>(2,2), T.at<double>(2,0));
				mright.setLocalTransform(mleft.localTransform() * extrinsics.inverse());
			}
			else
			{
				Transform extrinsics(1, 0, 0, 0,
									 0, 1, 0, data.stereoCameraModel().baseline(),
									 0, 0, 1, 0);
				mright.setLocalTransform(extrinsics * mleft.localTransform());
			}


			models.push_back(mright);
		}
		else
		{
			UASSERT(int((data.imageRaw().cols/data.cameraModels().size())*data.cameraModels().size()) == data.imageRaw().cols);
			int subImageWidth = data.imageRaw().cols/data.cameraModels().size();
			for(unsigned int i=0; i<data.cameraModels().size(); ++i)
			{
				if(data.cameraModels()[i].isValidForProjection())
				{
					images.push_back(cv::Mat(data.imageRaw(), cv::Rect(subImageWidth*i, 0, subImageWidth, data.imageRaw().rows)));
					CameraModel m = data.cameraModels()[i];
					// should be transform between IMU and camera
					m.setLocalTransform(lastImu_.localTransform().inverse()*m.localTransform());
					models.push_back(m);
				}
			}
		}

		if(images.size())
		{
			// initialization
			if(okvisEstimator_ == 0)
			{
				UDEBUG("Initialization");
				if(lastImu_.empty())
				{
					UWARN("Ignoring Image, waiting for imu to initialize...");
					return t;
				}

				okvis::VioParameters parameters;
				if(configFilename_.empty())
				{
					UERROR("OKVIS config file is empty (%s)!", Parameters::kOdomOKVISConfigPath().c_str());
					return t;
				}
				else
				{
					okvis::VioParametersReader vio_parameters_reader(configFilename_);
					vio_parameters_reader.getParameters(parameters);
					if(parameters.nCameraSystem.numCameras() > 0)
					{
						UWARN("Camera calibration included in OKVIS is ignored as calibration from received images will be used instead.");
					}
					parameters.nCameraSystem = okvis::cameras::NCameraSystem();
				}

				parameters.publishing.publishRate = parameters.imu.rate; // rate at which odometry updates are published only works properly if imu_rate/publish_rate is an integer!!
				parameters.publishing.publishLandmarks = true; // select, if you want to publish landmarks at all
				parameters.publishing.publishImuPropagatedState = true; // Should the state that is propagated with IMU messages be published? Or just the optimized ones?
				parameters.publishing.landmarkQualityThreshold = 1.0e-2; // landmark with lower quality will not be published
				parameters.publishing.maxLandmarkQuality = 0.05; // landmark with higher quality will be published with the maximum colour intensity
				parameters.publishing.trackedBodyFrame = okvis::FrameName::B; // B or S, the frame of reference that will be expressed relative to the selected worldFrame
				parameters.publishing.velocitiesFrame = okvis::FrameName::B; // Wc, B or S,  the frames in which the velocities of the selected trackedBodyFrame will be expressed in

				// non-hard coded parameters
				parameters.imu.T_BS = okvis::kinematics::Transformation(lastImu_.localTransform().toEigen4d());
				for(unsigned int i=0; i<models.size(); ++i)
				{
					okvis::cameras::NCameraSystem::DistortionType distType = okvis::cameras::NCameraSystem::NoDistortion;
					std::shared_ptr<const okvis::cameras::CameraBase> cam;
					if(!imagesAlreadyRectified())
					{
						if(models[i].D_raw().cols == 8)
						{
							okvis::cameras::RadialTangentialDistortion8 dist(
									models[i].D_raw().at<double>(0,0),
									models[i].D_raw().at<double>(0,1),
									models[i].D_raw().at<double>(0,2),
									models[i].D_raw().at<double>(0,3),
									models[i].D_raw().at<double>(0,4),
									models[i].D_raw().at<double>(0,5),
									models[i].D_raw().at<double>(0,6),
									models[i].D_raw().at<double>(0,7));
							cam.reset(
									new okvis::cameras::PinholeCamera<okvis::cameras::RadialTangentialDistortion8>(
											models[i].imageWidth(),
											models[i].imageHeight(),
											models[i].K_raw().at<double>(0,0),
											models[i].K_raw().at<double>(1,1),
											models[i].K_raw().at<double>(0,2),
											models[i].K_raw().at<double>(1,2),
											dist));
							distType = okvis::cameras::NCameraSystem::RadialTangential8;
							UINFO("RadialTangential8");
						}
						else if(models[i].D_raw().cols == 6)
						{
							okvis::cameras::EquidistantDistortion dist(
									models[i].D_raw().at<double>(0,0),
									models[i].D_raw().at<double>(0,1),
									models[i].D_raw().at<double>(0,4),
									models[i].D_raw().at<double>(0,5));
							cam.reset(new okvis::cameras::PinholeCamera<okvis::cameras::EquidistantDistortion>(
											models[i].imageWidth(),
											models[i].imageHeight(),
											models[i].K_raw().at<double>(0,0),
											models[i].K_raw().at<double>(1,1),
											models[i].K_raw().at<double>(0,2),
											models[i].K_raw().at<double>(1,2),
											dist));
							distType = okvis::cameras::NCameraSystem::Equidistant;
							UINFO("Equidistant");
						}
						else if(models[i].D_raw().cols >= 4)
						{
							// To support not rectified images
							okvis::cameras::RadialTangentialDistortion dist(
									models[i].D_raw().at<double>(0,0),
									models[i].D_raw().at<double>(0,1),
									models[i].D_raw().at<double>(0,2),
									models[i].D_raw().at<double>(0,3));
							cam.reset(
									new okvis::cameras::PinholeCamera<okvis::cameras::RadialTangentialDistortion>(
											models[i].imageWidth(),
											models[i].imageHeight(),
											models[i].K_raw().at<double>(0,0),
											models[i].K_raw().at<double>(1,1),
											models[i].K_raw().at<double>(0,2),
											models[i].K_raw().at<double>(1,2),
											dist));
							distType = okvis::cameras::NCameraSystem::RadialTangential;
							UINFO("RadialTangential");
						}
					}
					else // no distortion, rectified images
					{
						okvis::cameras::RadialTangentialDistortion dist(0,0,0,0);
						cam.reset(
								new okvis::cameras::PinholeCamera<okvis::cameras::RadialTangentialDistortion>(
										models[i].imageWidth(),
										models[i].imageHeight(),
										models[i].K().at<double>(0,0),
										models[i].K().at<double>(1,1),
										models[i].K().at<double>(0,2),
										models[i].K().at<double>(1,2),
										dist));
						distType = okvis::cameras::NCameraSystem::RadialTangential;
					}

					if(cam.get())
					{
						UINFO("model %d: %s", i, models[i].localTransform().prettyPrint().c_str());

						Eigen::Vector3d r(models[i].localTransform().x(), models[i].localTransform().y(), models[i].localTransform().z());
						parameters.nCameraSystem.addCamera(
								std::shared_ptr<const okvis::kinematics::Transformation>(new okvis::kinematics::Transformation(r, models[i].localTransform().getQuaterniond().normalized())),
								cam,
								distType);
					}
				}

				okvisEstimator_ = new okvis::ThreadedKFVio(parameters);

				okvisEstimator_->setFullStateCallback(
				  std::bind(&OkvisCallbackHandler::fullStateCallback, okvisCallbackHandler_,
							std::placeholders::_1, std::placeholders::_2,
							std::placeholders::_3, std::placeholders::_4));

				okvisEstimator_->setLandmarksCallback(
				  std::bind(&OkvisCallbackHandler::landmarksCallback, okvisCallbackHandler_,
							std::placeholders::_1, std::placeholders::_2,
							std::placeholders::_3));

				okvisEstimator_->setBlocking(true);
			}

			for(unsigned int i=0; i<images.size(); ++i)
			{
				cv::Mat gray;
				if(images[i].type() == CV_8UC3)
				{
					cv::cvtColor(images[i], gray, CV_BGR2GRAY);
				}
				else if(images[i].type() == CV_8UC1)
				{
					gray = images[i];
				}
				else
				{
					UFATAL("Not supported color type!");
				}

				imageUpdated = okvisEstimator_->addImage(timeOkvis, i, gray);
				if(!imageUpdated)
				{
					UWARN("Image update with stamp %f delayed...", data.stamp());
				}
			}
			if(imageUpdated)
			{
				++imagesProcessed_;
			}
		}
	}

	if((imageUpdated || imuUpdated) && imagesProcessed_ > 10)
	{
		Transform fixPos(-1,0,0,0, 0,-1,0,0, 0,0,1,0);
		Transform fixRot(0,0,1,0, 0,-1,0,0, 1,0,0,0);
		Transform p = okvisCallbackHandler_->getLastTransform();
		if(!p.isNull())
		{
			p = fixPos * p * fixRot;
			// make it incremental
			t = this->getPose().inverse()*p;

			if(info)
			{
				info->reg.covariance = cv::Mat::eye(6,6, CV_64FC1);
				info->reg.covariance *= this->framesProcessed() == 0?9999:0.0001;

				// FIXME: the scale of landmarks doesn't seem to fit well the environment...
				/*info->localMap = okvisCallbackHandler_->getLastLandmarks();
				info->localMapSize = info->localMap.size();
				for(std::map<int, cv::Point3f>::iterator iter=info->localMap.begin(); iter!=info->localMap.end(); ++iter)
				{
					iter->second = util3d::transformPoint(iter->second, fixPos);
				}*/
			}
		}
		UINFO("Odom update time = %fs p=%s", timer.elapsed(), p.prettyPrint().c_str());
	}
#else
	UERROR("RTAB-Map is not built with OKVIS support! Select another visual odometry approach.");
#endif
	return t;
}

} // namespace rtabmap
