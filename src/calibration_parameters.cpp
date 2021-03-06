#include "plane_calibration/calibration_parameters.hpp"

namespace plane_calibration
{
CalibrationParameters::CalibrationParameters(bool precompute_planes, int precomputed_plane_pairs_count)
{
  parameters_.precompute_planes_ = precompute_planes;
  parameters_.precomputed_plane_pairs_count_ = precomputed_plane_pairs_count;
  updated_ = true;
}

CalibrationParameters::CalibrationParameters(const double& max_deviation, const Eigen::Vector3d& ground_plane_offset,
                                             const Eigen::AngleAxisd& rotation, bool precompute_planes,
                                             int precomputed_plane_pairs_count)
{
  parameters_.ground_plane_offset_ = ground_plane_offset;
  parameters_.max_deviation_ = max_deviation;
  parameters_.deviation_ = parameters_.max_deviation_;
  parameters_.rotation_ = rotation;

  parameters_.precompute_planes_ = precompute_planes;
  parameters_.precomputed_plane_pairs_count_ = precomputed_plane_pairs_count;
  updated_ = true;
}

CalibrationParameters::CalibrationParameters(const CalibrationParameters &object)
{
  std::lock_guard<std::mutex> lock(object.mutex_);
  parameters_ = object.parameters_;
  updated_ = object.updated_;
}

bool CalibrationParameters::getUpdatedParameters(Parameters& updated_parameters)
{
  std::lock_guard<std::mutex> lock(mutex_);
  bool updated = updated_;
  updated_parameters = parameters_;
  updated_ = false;
  return updated;
}

CalibrationParameters::Parameters CalibrationParameters::getParameters()
{
  std::lock_guard<std::mutex> lock(mutex_);
  return parameters_;
}

bool CalibrationParameters::parametersUpdated()
{
  std::lock_guard<std::mutex> lock(mutex_);
  return updated_;
}

Eigen::Affine3d CalibrationParameters::getTransform() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return parameters_.getTransform();
}

void CalibrationParameters::update(const Eigen::Vector3d& ground_plane_offset, const double& max_deviation,
                                   const Eigen::AngleAxisd& rotation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.ground_plane_offset_ = ground_plane_offset;
  parameters_.max_deviation_ = max_deviation;
  parameters_.deviation_ = parameters_.max_deviation_;
  parameters_.rotation_ = rotation;
  updated_ = true;
}

void CalibrationParameters::update(const Eigen::Vector3d& ground_plane_offset, const Eigen::AngleAxisd& rotation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.ground_plane_offset_ = ground_plane_offset;
  parameters_.rotation_ = rotation;
  updated_ = true;
}

void CalibrationParameters::update(const Parameters& parameters)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_ = parameters;
  updated_ = true;
}

void CalibrationParameters::update(const double& deviation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.deviation_ = deviation;
  updated_ = true;
}

void CalibrationParameters::update(const Eigen::Vector3d& ground_plane_offset)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.ground_plane_offset_ = ground_plane_offset;
  updated_ = true;
}

void CalibrationParameters::update(const Eigen::AngleAxisd& rotation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.rotation_ = rotation;
  updated_ = true;
}

void CalibrationParameters::updateDeviations(const double& value)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.max_deviation_ = value;
  parameters_.deviation_ = value;
  updated_ = true;
}

void CalibrationParameters::updatePrecomputation(const bool& enable, const int& plane_pair_count)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.precompute_planes_ = enable;
  parameters_.precomputed_plane_pairs_count_ = plane_pair_count;
  updated_ = true;
}

void CalibrationParameters::updateDeviation(const double& deviation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  parameters_.deviation_ = deviation;
  updated_ = true;
}

} /* end namespace */
