/*
 * IMU.h
 *
 *  Created on: 2018-03-05
 *      Author: mathieu
 */

#ifndef IMU_H_
#define IMU_H_

#include <opencv2/core.hpp>
#include <rtabmap/utilite/ULogger.h>

namespace rtabmap {


// Correspondence class to sensor_msgs/IMU
class IMU
{
public:
	IMU() {}
	IMU(const Eigen::Quaterniond & orientation,
		const cv::Mat & orientationCovariance,
		const Eigen::Vector3d & angularVelocity,
		const cv::Mat & angularVelocityCovariance,
		const Eigen::Vector3d & linearAcceleration,
		const cv::Mat & linearAccelerationCovariance,
		const Transform & localTransform = Transform::getIdentity()) :
			orientation_(orientation),
			orientationCovariance_(orientationCovariance),
			angularVelocity_(angularVelocity),
			angularVelocityCovariance_(angularVelocityCovariance),
			linearAcceleration_(linearAcceleration),
			linearAccelerationCovariance_(linearAccelerationCovariance),
			localTransform_(localTransform)
	{
		UASSERT(!orientationCovariance.empty() && orientationCovariance.cols == 3 && orientationCovariance.rows == 3 && orientationCovariance.type() == CV_64FC1);
		UASSERT(!angularVelocityCovariance.empty() && angularVelocityCovariance.cols == 3 && angularVelocityCovariance.rows == 3 && angularVelocityCovariance.type() == CV_64FC1);
		UASSERT(!linearAccelerationCovariance.empty() && linearAccelerationCovariance.cols == 3 && linearAccelerationCovariance.rows == 3 && linearAccelerationCovariance.type() == CV_64FC1);
	}
	IMU(const Eigen::Vector3d & angularVelocity,
		const cv::Mat & angularVelocityCovariance,
		const Eigen::Vector3d & linearAcceleration,
		const cv::Mat & linearAccelerationCovariance,
		const Transform & localTransform = Transform::getIdentity()) :
			angularVelocity_(angularVelocity),
			angularVelocityCovariance_(angularVelocityCovariance),
			linearAcceleration_(linearAcceleration),
			linearAccelerationCovariance_(linearAccelerationCovariance),
			localTransform_(localTransform)
	{
		UASSERT(!angularVelocityCovariance.empty() && angularVelocityCovariance.cols == 3 && angularVelocityCovariance.rows == 3 && angularVelocityCovariance.type() == CV_64FC1);
		UASSERT(!linearAccelerationCovariance.empty() && linearAccelerationCovariance.cols == 3 && linearAccelerationCovariance.rows == 3 && linearAccelerationCovariance.type() == CV_64FC1);
	}

	const Eigen::Quaterniond & orientation() const {return orientation_;}
	const cv::Mat & orientationCovariance() const {return orientationCovariance_;} // 3x3 double Row major about x, y, z axes, empty if orientation is not set

	const Eigen::Vector3d & angularVelocity() const {return angularVelocity_;}
	const cv::Mat & angularVelocityCovariance() const {return angularVelocityCovariance_;} // 3x3 double Row major about x, y, z axes, empty if angularVelocity is not set

	const Eigen::Vector3d linearAcceleration() const {return linearAcceleration_;}
	const cv::Mat & linearAccelerationCovariance() const {return linearAccelerationCovariance_;} // 3x3 double Row major x, y z, empty if linearAcceleration is not set

	const Transform & localTransform() const {return localTransform_;}

	bool empty() const
	{
		return orientationCovariance_.empty() && angularVelocityCovariance_.empty() && linearAccelerationCovariance_.empty();
	}


private:
	Eigen::Quaterniond orientation_;
	cv::Mat orientationCovariance_; // 3x3 double Row major about x, y, z axes, empty if orientation is not set

	Eigen::Vector3d angularVelocity_;
	cv::Mat angularVelocityCovariance_; // 3x3 double Row major about x, y, z axes, empty if angularVelocity is not set

	Eigen::Vector3d linearAcceleration_;
	cv::Mat linearAccelerationCovariance_; // 3x3 double Row major x, y z, empty if linearAcceleration is not set

	Transform localTransform_;
};

class IMUEvent : public UEvent
{
public:
	IMUEvent() :
		stamp_(0.0)
	{}
	IMUEvent(const IMU & data, double stamp) :
		data_(data),
		stamp_(stamp)
	{
	}
	virtual std::string getClassName() const {return "IMUEvent";}
	const IMU & getData() const {return data_;}
	double getStamp() const {return stamp_;}

private:
	IMU data_;
	double stamp_;
};

}


#endif /* IMU_H_ */
