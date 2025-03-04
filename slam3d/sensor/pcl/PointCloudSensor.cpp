// slam3d - Frontend for graph-based SLAM
// Copyright (C) 2017 S. Kasperski
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
// TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "PointCloudSensor.hpp"

#include <slam3d/core/Mapper.hpp>

#include <pclomp/gicp_omp.h>
#include <pclomp/ndt_omp.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/pcl_config.h>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model_plane.h>

#include <boost/format.hpp>

#define PI 3.141592654

using namespace slam3d;

PointCloudSensor::PointCloudSensor(const std::string& n, Logger* l)
 : ScanSensor(n, l)
{
	mMapResolution = 0.1;
	mMapOutlierRadius = 0.2;
	mMapOutlierNeighbors = 3;
}

PointCloudSensor::~PointCloudSensor()
{

}

PointCloud::Ptr PointCloudSensor::downsample(PointCloud::ConstPtr in, double leaf_size) const
{
	PointCloud::Ptr out(new PointCloud);
	if(in->size() > 0)
	{
		pcl::VoxelGrid<PointType> grid;
		grid.setLeafSize (leaf_size, leaf_size, leaf_size);
		grid.setInputCloud(in);
		grid.filter(*out);
	}
	return out;
}

PointCloud::Ptr PointCloudSensor::removeOutliers(PointCloud::ConstPtr in, double radius, unsigned min_neighbors) const
{
	PointCloud::Ptr out(new PointCloud);
	if(in->size() > 0)
	{
		pcl::RadiusOutlierRemoval<PointType> out_removal;
		out_removal.setInputCloud(in);
		out_removal.setRadiusSearch(radius);
		out_removal.setMinNeighborsInRadius(min_neighbors);
		out_removal.filter(*out);
	}
	return out;
}

PointCloud::Ptr PointCloudSensor::transform(PointCloud::ConstPtr source, const Transform tf) const
{
	PointCloud::Ptr transformedCloud(new PointCloud);
	pcl::transformPointCloud(*source, *transformedCloud, tf.matrix());
	return transformedCloud;
}

PointCloud::Ptr PointCloudSensor::getAccumulatedCloud(const VertexObjectList& vertices) const
{
	PointCloud::Ptr accu(new PointCloud);
	for(VertexObjectList::const_reverse_iterator it = vertices.rbegin(); it != vertices.rend(); it++)
	{
		PointCloudMeasurement::Ptr pcl = boost::dynamic_pointer_cast<PointCloudMeasurement>(it->measurement);
		if(!pcl)
		{
			mLogger->message(ERROR, "Measurement in getAccumulatedCloud() is not a point cloud!");
			throw BadMeasurementType();
		}
		
		PointCloud::Ptr tempCloud = transform(pcl->getPointCloud(), (it->corrected_pose * pcl->getSensorPose()));
		*accu += *tempCloud;
	}
	return accu;
}

Measurement::Ptr PointCloudSensor::createCombinedMeasurement(const VertexObjectList& vertices, Transform pose) const
{
	PointCloud::Ptr cloud = getAccumulatedCloud(vertices);
	PointCloud::Ptr shifted(new PointCloud);
	pcl::transformPointCloud(*cloud, *shifted, pose.inverse().matrix());
	mLogger->message(DEBUG, (boost::format("Patch pointcloud has %1% points.") % cloud->size()).str());
	Measurement::Ptr m(new PointCloudMeasurement(shifted, "AccumulatedPointcloud", mName, Transform::Identity()));
	return m;
}


Constraint::Ptr PointCloudSensor::createConstraint(const Measurement::Ptr& source,
                                                   const Measurement::Ptr& target,
                                                   const Transform& odometry, bool loop)
{
	// Transform guess in sensor frame
	Transform guess = source->getInverseSensorPose() * odometry * target->getSensorPose();
	
	// Cast to this sensors measurement type
	PointCloudMeasurement::Ptr sourceCloud = boost::dynamic_pointer_cast<PointCloudMeasurement>(source);
	PointCloudMeasurement::Ptr targetCloud = boost::dynamic_pointer_cast<PointCloudMeasurement>(target);
	if(!sourceCloud || !targetCloud)
	{
		mLogger->message(ERROR, "Measurement given to createConstraint() is not a PointCloud!");
		throw BadMeasurementType();
	}
	
	// For large loops, refine guess by a coarse ICP
	if(loop)
	{
		guess = align(sourceCloud, targetCloud, guess, mCoarseConfiguration);
	}
	
	// Calculate precise alignement with fine ICP
	Transform icp_result = align(sourceCloud, targetCloud, guess, mFineConfiguration);
	
	// Transform back to robot frame
	Transform transform = source->getSensorPose() * icp_result * target->getInverseSensorPose();
	Covariance<6> covariance = Covariance<6>::Identity() * mCovarianceScale;
	
	return Constraint::Ptr(new SE3Constraint(mName, transform, covariance.inverse()));
}

Transform PointCloudSensor::align(PointCloudMeasurement::Ptr source,
                                  PointCloudMeasurement::Ptr target,
                                  const Transform& guess,
                                  const RegistrationParameters& config)
{
	// Downsample the scans
	PointCloud::Ptr filtered_source = source->getPointCloud();
	PointCloud::Ptr filtered_target = target->getPointCloud();
	if(config.point_cloud_density > 0)
	{
		PointCloud::Ptr filtered_source = downsample(source->getPointCloud(), config.point_cloud_density);
		PointCloud::Ptr filtered_target = downsample(target->getPointCloud(), config.point_cloud_density);
	}
	
	// Make sure that there are enough points left (ICP will crash if not)
	if(filtered_target->size() < 100 || filtered_source->size() < 100)
		throw NoMatch("Too few points after filtering, you may have to decrease 'point_cloud_density'.");
	
	// Configure Generalized-ICP
	if(config.registration_algorithm == GICP)
	{
		return doICP(filtered_source, filtered_target, guess, config);
	}else
	{
		return doNDT(filtered_source, filtered_target, guess, config);
	}
}

Transform PointCloudSensor::doICP(PointCloud::Ptr source,
                                  PointCloud::Ptr target,
                                  const Transform& guess,
                                  const RegistrationParameters& config)
{
	pclomp::GeneralizedIterativeClosestPoint<PointType, PointType> icp;
	icp.setMaxCorrespondenceDistance(config.max_correspondence_distance);
	icp.setMaximumIterations(config.maximum_iterations);
	icp.setTransformationEpsilon(config.transformation_epsilon);
	icp.setEuclideanFitnessEpsilon(config.euclidean_fitness_epsilon);
	icp.setCorrespondenceRandomness(config.correspondence_randomness);
	icp.setMaximumOptimizerIterations(config.maximum_optimizer_iterations);
	icp.setRotationEpsilon(config.rotation_epsilon);
	
	PointCloud result;

#if PCL_VERSION_COMPARE(<, 1, 8, 1)
	// We cannot use the "guess" parameter from align() due to a bug in PCL.
	// Instead we have to shift the source cloud to the target frame before
	// calling align on it.
	// > https://github.com/PointCloudLibrary/pcl/pull/989
	PointCloud::Ptr shifted_target(new PointCloud);
	pcl::transformPointCloud(*target, *shifted_target, guess.matrix());
	
	// Source and target are switched at this point!
	// In the pose graph, our edge (with transform) goes from source to target,
	// but ICP calculates the transformation from target to source.
	icp.setInputSource(shifted_target);
	icp.setInputTarget(source);
	icp.align(result);
#else
	icp.setInputSource(target);
	icp.setInputTarget(source);
	icp.align(result, guess.matrix().cast<float>());
#endif

	// Check if ICP was successful (kind of...)
	double score = icp.getFitnessScore(config.max_correspondence_distance);
	if(!icp.hasConverged() || score > config.max_fitness_score)
	{
		throw NoMatch((boost::format("ICP failed with Fitness-Score %1% > %2%") % score % config.max_fitness_score).str());
	}
	
	// Get estimated transform
	Transform icp_result(Eigen::Isometry3f(icp.getFinalTransformation()));
#if PCL_VERSION_COMPARE(<, 1, 8, 1)
	icp_result = icp_result * guess;
#endif
	return icp_result;
}

Transform PointCloudSensor::doNDT(PointCloud::Ptr source,
                                  PointCloud::Ptr target,
                                  const Transform& guess,
                                  const RegistrationParameters& config)
{
	pclomp::NormalDistributionsTransform<PointType, PointType> ndt;
	ndt.setMaxCorrespondenceDistance(config.max_correspondence_distance);
	ndt.setMaximumIterations(config.maximum_iterations);
	ndt.setTransformationEpsilon(config.transformation_epsilon);
	ndt.setEuclideanFitnessEpsilon(config.euclidean_fitness_epsilon);
	ndt.setOutlierRatio(config.outlier_ratio);
	ndt.setStepSize(config.step_size);
	ndt.setResolution(config.resolution);
	
	// Source and target are switched at this point!
	// In the pose graph, our edge (with transform) goes from source to target,
	// but ICP calculates the transformation from target to source.
	ndt.setInputSource(target);
	ndt.setInputTarget(source);
	PointCloud result;
	ndt.align(result, guess.matrix().cast<float>());

	// Check if NDT was successful (kind of...)
	double score = ndt.getFitnessScore(config.max_correspondence_distance);
	mLogger->message(DEBUG, (boost::format("NDT: fitness(%1%) probability(%2%) iterations(%3%)")
		%score % ndt.getTransformationProbability() % ndt.getFinalNumIteration()).str());
	if(!ndt.hasConverged() || score > config.max_fitness_score)
	{
		throw NoMatch((boost::format("NDT failed with Fitness-Score %1% > %2%") % score % config.max_fitness_score).str());
	}
	
	// Get estimated transform
	Eigen::Isometry3f tf_matrix(ndt.getFinalTransformation());
	return Transform(tf_matrix);
}

PointCloud::Ptr PointCloudSensor::buildMap(const VertexObjectList& vertices) const
{
	PointCloud::Ptr accu = getAccumulatedCloud(vertices);
	PointCloud::Ptr cleaned = removeOutliers(accu, mMapOutlierRadius, mMapOutlierNeighbors);
	return downsample(cleaned, mMapResolution);
}

void PointCloudSensor::setRegistrationParameters(const RegistrationParameters& conf, bool coarse)
{
	if(coarse)
	{
		mLogger->message(INFO, " = RegistrationParameters (Coarse) =");
		mCoarseConfiguration = conf;
	}else
	{
		mLogger->message(INFO, " = RegistrationParameters (Fine) =");
		mFineConfiguration = conf;
	}
	mLogger->message(INFO, (boost::format("correspondence_randomness:    %1%") % conf.correspondence_randomness).str());
	mLogger->message(INFO, (boost::format("euclidean_fitness_epsilon:    %1%") % conf.euclidean_fitness_epsilon).str());
	mLogger->message(INFO, (boost::format("max_correspondence_distance:  %1%") % conf.max_correspondence_distance).str());
	mLogger->message(INFO, (boost::format("max_fitness_score:            %1%") % conf.max_fitness_score).str());
	mLogger->message(INFO, (boost::format("maximum_iterations:           %1%") % conf.maximum_iterations).str());
	mLogger->message(INFO, (boost::format("maximum_optimizer_iterations: %1%") % conf.maximum_optimizer_iterations).str());
	mLogger->message(INFO, (boost::format("point_cloud_density:          %1%") % conf.point_cloud_density).str());
	mLogger->message(INFO, (boost::format("rotation_epsilon:             %1%") % conf.rotation_epsilon).str());
	mLogger->message(INFO, (boost::format("transformation_epsilon:       %1%") % conf.transformation_epsilon).str());
}

void PointCloudSensor::setMapResolution(double r)
{
	mLogger->message(INFO, (boost::format("map_resolution:         %1%") % r).str());
	mMapResolution = r;
}

void PointCloudSensor::setMapOutlierRemoval(double r, unsigned n)
{
	mLogger->message(INFO, (boost::format("map_outlier_radius:     %1%") % r).str());
	mLogger->message(INFO, (boost::format("map_outlier_neighbors:  %1%") % n).str());
	mMapOutlierRadius = r;
	mMapOutlierNeighbors = n;
}

void PointCloudSensor::fillGroundPlane(PointCloud::Ptr cloud, ScalarType radius)
{
	pcl::SampleConsensusModelPlane<PointType>::Ptr
		model(new pcl::SampleConsensusModelPlane<PointType>(cloud));
	pcl::RandomSampleConsensus<PointType> ransac(model);
	ransac.setDistanceThreshold (.01);
	ransac.computeModel();

	Eigen::VectorXf c;
	ransac.getModelCoefficients(c);
	Direction normal(c[0], c[1], c[2]);
	Eigen::Hyperplane<ScalarType, 3> plane(normal, c[3]);
	double angle_inc = mMapResolution / radius;
	for(ScalarType r = mMapResolution; r <= radius; r += mMapResolution)
	{
		Position sample = plane.projection(Position(r,0,0));
		for(ScalarType angle = 0; angle < 2*PI; angle += angle_inc)
		{
			Position rot = Eigen::AngleAxis<ScalarType>(angle, normal).toRotationMatrix() * sample;
			PointType p;
			p.x = rot[0];
			p.y = rot[1];
			p.z = rot[2];
			cloud->push_back(p);
		}
	}
}
