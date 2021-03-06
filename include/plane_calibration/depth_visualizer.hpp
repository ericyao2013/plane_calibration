#ifndef plane_calibration_SRC_DEPTH_VISUALIZER_HPP_
#define plane_calibration_SRC_DEPTH_VISUALIZER_HPP_

#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <Eigen/Dense>
#include <ros/node_handle.h>
#include <ros/publisher.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/PointCloud2.h>
#include <image_geometry/pinhole_camera_model.h>

#include "camera_model.hpp"
#include "visualizer_interface.hpp"

namespace plane_calibration
{

class DepthVisualizer : public VisualizerInterface
{
public:
  DepthVisualizer(ros::NodeHandle node_handle, std::string frame_id = std::string("camera_depth_optical_frame"));

  virtual void setCameraModel(const image_geometry::PinholeCameraModel& camera_model);

  virtual void publishImage(const std::string& topic, const Eigen::MatrixXf& image_matrix, std::string frame_id =
                                std::string(""));
  virtual void publishImage(const std::string& topic, const sensor_msgs::Image& image_msg);
  virtual void publishCloud(const std::string& topic, const Eigen::Affine3d& plane_transformation,
                            const CameraModel::Parameters& camera_model_paramaters,
                            std::string frame_id = std::string(""));
  virtual void publishCloud(const std::string& topic, const Eigen::MatrixXf& image_matrix, std::string frame_id =
                                std::string(""));
  virtual void publishCloud(const std::string& topic, const sensor_msgs::Image& image_msg);

  virtual void publishDouble(const std::string& topic, const double& value);

  static sensor_msgs::PointCloud2Ptr floatImageMsgToPointCloud(const sensor_msgs::Image& image_msg,
                                                               const image_geometry::PinholeCameraModel& camera_model);

protected:
  template<typename MsgType>
  void addPublisherIfNotExist(const std::string& topic);
  sensor_msgs::PointCloud2Ptr imageMsgToPointCloud(const sensor_msgs::Image& image_msg);

  ros::NodeHandle node_handle_;
  std::string frame_id_;
  std::mutex camera_model_mutex_;
  image_geometry::PinholeCameraModel camera_model_;
  std::map<std::string, ros::Publisher> publishers_;
};

} /* end namespace */

#endif
