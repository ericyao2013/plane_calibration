#ifndef plane_calibration_SRC_PLANE_CALIBRATION_NODELET_HPP_
#define plane_calibration_SRC_PLANE_CALIBRATION_NODELET_HPP_

#include <atomic>
#include <mutex>
#include <memory>
#include <Eigen/Dense>

#include <nodelet/nodelet.h>
#include <dynamic_reconfigure/server.h>
#include <plane_calibration/PlaneCalibrationConfig.h>
#include <ros/time.h>
#include <ros/subscriber.h>
#include <ros/publisher.h>
#include <sensor_msgs/Image.h>
#include <tf2_ros/transform_listener.h>
#include <geometry_msgs/TransformStamped.h>

#include "camera_model.hpp"
#include "calibration_parameters.hpp"
#include "input_filter.hpp"
#include "plane_calibration.hpp"
#include "calibration_validation.hpp"
#include "plane_to_depth_image.hpp"
#include "depth_visualizer.hpp"

namespace plane_calibration
{

class PlaneCalibrationNodelet : public nodelet::Nodelet
{
public:
  PlaneCalibrationNodelet();

  virtual void onInit();

protected:
  virtual void reconfigureCB(PlaneCalibrationConfig &config, uint32_t level);
  virtual void cameraInfoCB(const sensor_msgs::CameraInfoConstPtr& camera_info_msg);
  virtual void depthImageCB(const sensor_msgs::ImageConstPtr& depth_image_msg);

  virtual void getTransform();
  virtual std::pair<Eigen::Vector3d, Eigen::AngleAxisd> getTransformManual();
  virtual std::pair<Eigen::Vector3d, Eigen::AngleAxisd> getTransformTF();

  virtual void runCalibration(Eigen::MatrixXf depth_matrix);
  virtual void publishTransform();

  std::atomic<bool> enable_;
  ros::Time last_call_time_;
  double calibration_rate_;

  bool precompute_planes_;
  int precomputed_plane_pairs_count_;
  CameraModelPtr camera_model_;
  CalibrationParametersPtr calibration_parameters_;

  tf2_ros::Buffer transform_listener_buffer_;
  tf2_ros::TransformListener transform_listener_;
  std::shared_ptr<std::pair<Eigen::Vector3d, Eigen::AngleAxisd>> transform_;

  InputFilterPtr input_filter_;
  PlaneCalibrationPtr plane_calibration_;
  CalibrationValidationPtr calibration_validation_;
  PlaneToDepthImagePtr plane_to_depth_converter_;

  std::atomic<double> max_deviation_;
  Eigen::Vector3d ground_plane_offset_;
  Eigen::AngleAxisd ground_plane_rotation_;
  std::atomic<int> iterations_;

  std::pair<double, double> last_valid_calibration_result_;
  std::pair<double, double> last_calibration_result_;
  Eigen::MatrixXf last_valid_calibration_result_plane_;
  Eigen::Affine3d last_valid_calibration_transformation_;

  InputFilter::Config input_filter_config_;
  CalibrationValidation::Config calibration_validation_config_;

  std::shared_ptr<DepthVisualizer> depth_visualizer_;
  std::shared_ptr<dynamic_reconfigure::Server<PlaneCalibrationConfig>> reconfigure_server_;

  std::atomic<bool> debug_;
  std::atomic<bool> use_manual_ground_transform_;
  std::atomic<bool> always_update_;
  std::atomic<double> x_offset_;
  std::atomic<double> y_offset_;
  std::atomic<double> z_offset_;

  std::atomic<double> px_offset_;
  std::atomic<double> py_offset_;
  std::atomic<double> pz_offset_;

  std::string ground_frame_;
  std::string camera_depth_frame_;
  std::string result_frame_;

  ros::Publisher pub_update_;
  ros::Subscriber sub_camera_info_;
  ros::Subscriber sub_depth_image_;
  
  // some parameter for calibration
  float maximum_range_of_depth_camera;
  float threshold_normalized_z_by_xy;
  float threshold_normalized_invalid_z_by_xy;
  float max_angle_change;
};

} /* end namespace */

#endif
