#include "plane_calibration/plane_calibration_nodelet.hpp"

#include <sstream>
#include <Eigen/Dense>
#include <ecl/geometry/angle.hpp>

#include <pluginlib/class_list_macros.h>
#include <ros/node_handle.h>
#include <tf2_msgs/TFMessage.h>
#include <image_geometry/pinhole_camera_model.h>
#include <eigen_conversions/eigen_msg.h>
#include <tf2_ros/transform_broadcaster.h>
#include <geometry_msgs/Pose2D.h>

#include "plane_calibration/image_msg_eigen_converter.hpp"

namespace plane_calibration
{

PlaneCalibrationNodelet::PlaneCalibrationNodelet() :
    transform_listener_buffer_(), transform_listener_(transform_listener_buffer_)
{
  enable_ = true;
  calibration_rate_ = 1.0;
  debug_ = false;
  use_manual_ground_transform_ = false;
  always_update_ = false;
  ground_plane_rotation_ = Eigen::AngleAxisd::Identity();

  precompute_planes_ = true;
  precomputed_plane_pairs_count_ = 20;

  last_valid_calibration_result_ = std::make_pair(0.0, 0.0);

  Eigen::AngleAxisd rotation;
  rotation = Eigen::AngleAxisd(last_valid_calibration_result_.first, Eigen::Vector3d::UnitX())
      * Eigen::AngleAxisd(last_valid_calibration_result_.second, Eigen::Vector3d::UnitY());

  last_valid_calibration_transformation_ = Eigen::Translation3d(0.0, 0.0, 0.0) * rotation;
  
  last_calibration_result_.first = 0;
  last_calibration_result_.second = 0;
}

void PlaneCalibrationNodelet::onInit()
{
  ROS_INFO("[PlaneCalibrationNodelet]: Initializing");
  ros::NodeHandle node_handle = this->getPrivateNodeHandle();

  reconfigure_server_ = std::shared_ptr<dynamic_reconfigure::Server<PlaneCalibrationConfig> >(
      new dynamic_reconfigure::Server<PlaneCalibrationConfig>(node_handle));
  dynamic_reconfigure::Server<PlaneCalibrationConfig>::CallbackType reconfigure_cb = boost::bind(
      &PlaneCalibrationNodelet::reconfigureCB, this, _1, _2);
  reconfigure_server_->setCallback(reconfigure_cb);

  node_handle.param("calibration_rate", calibration_rate_, 1.0);
  node_handle.param("ground_frame", ground_frame_, std::string("base_footprint"));
  node_handle.param("precompute_planes", precompute_planes_, true);
  node_handle.param("precomputed_plane_pairs_count", precomputed_plane_pairs_count_, 20);

  node_handle.param("camera_depth_frame", camera_depth_frame_, std::string("camera_depth_optical_frame"));
  node_handle.param("result_camera_depth_frame", result_frame_, std::string("ground_plane_frame"));

  depth_visualizer_ = std::make_shared<DepthVisualizer>(node_handle, camera_depth_frame_);

  pub_update_ = node_handle.advertise<geometry_msgs::Pose2D>("plane_angle_update_degrees", 1);

  sub_camera_info_ = node_handle.subscribe<sensor_msgs::CameraInfo>("camera_info", 1,
                                                                    &PlaneCalibrationNodelet::cameraInfoCB, this);
  sub_depth_image_ = node_handle.subscribe<sensor_msgs::Image>("input_depth_image", 1,
                                                               &PlaneCalibrationNodelet::depthImageCB, this);

  ROS_INFO("[PlaneCalibrationNodelet]: Initialization finished");
}

void PlaneCalibrationNodelet::reconfigureCB(PlaneCalibrationConfig &config, uint32_t level)
{
  if (debug_ != config.debug)
  {
    debug_ = config.debug;
    std::string debug_status = debug_ ? "enabled" : "disabled";
    ROS_INFO_STREAM("[PlaneCalibrationNodelet]: Debug " << debug_status);
  }

  enable_ = config.enable;

  x_offset_ = config.x;
  y_offset_ = config.y;
  z_offset_ = config.z;

  px_offset_ = ecl::degrees_to_radians(config.px_degree);
  py_offset_ = ecl::degrees_to_radians(config.py_degree);
  pz_offset_ = ecl::degrees_to_radians(config.pz_degree);

  max_deviation_ = ecl::degrees_to_radians(config.max_deviation_degrees);
  iterations_ = config.iterations;

  input_filter_config_.max_nan_ratio = config.input_max_nan_ratio;
  input_filter_config_.max_zero_ratio = config.input_max_zero_ratio;
  input_filter_config_.min_data_ratio = config.input_min_data_ratio;
  input_filter_config_.max_error = config.input_max_noise;
  input_filter_config_.threshold_from_ground = config.input_threshold_from_ground;

  if (input_filter_)
  {
    input_filter_->updateConfig(input_filter_config_);
  }

  if (!calibration_parameters_)
  {
    calibration_parameters_ = std::make_shared<CalibrationParameters>(precompute_planes_,
                                                                      precomputed_plane_pairs_count_);
  }

  calibration_parameters_->updateDeviations(ecl::degrees_to_radians(config.max_deviation_degrees));
  calibration_parameters_->updatePrecomputation(config.precompute_planes, config.precomputed_plane_pairs_count);

  use_manual_ground_transform_ = config.use_manual_ground_transform;
  always_update_ = config.always_update;

  calibration_validation_config_.too_low_buffer = config.input_max_noise;
  calibration_validation_config_.max_too_low_ratio = config.plane_max_too_low_ratio;
  calibration_validation_config_.max_mean = config.plane_max_mean;

  if (calibration_validation_)
  {
    calibration_validation_->updateConfig(calibration_validation_config_);
  }
}

void PlaneCalibrationNodelet::cameraInfoCB(const sensor_msgs::CameraInfoConstPtr& camera_info_msg)
{
  image_geometry::PinholeCameraModel pinhole_camera_model;
  pinhole_camera_model.fromCameraInfo(camera_info_msg);

  depth_visualizer_->setCameraModel(pinhole_camera_model);

  if (!camera_model_)
  {
    camera_model_ = std::make_shared<CameraModel>();
  }

  camera_model_->update(pinhole_camera_model.cx(), pinhole_camera_model.cy(), pinhole_camera_model.fx(),
                        pinhole_camera_model.fy(), camera_info_msg->width, camera_info_msg->height);
}

void PlaneCalibrationNodelet::depthImageCB(const sensor_msgs::ImageConstPtr& depth_image_msg)
{
  ros::Time call_time = ros::Time::now();
  if (ros::Time::now() < last_call_time_ + ros::Duration(1.0 / calibration_rate_))
  {
    publishTransform();
    return;
  }
  last_call_time_ = ros::Time::now();

  if (!enable_)
  {
    if(debug_)
    {
      ROS_INFO_STREAM_THROTTLE(5.0, "[PlaneCalibrationNodelet]: Calibration disabled, not calibrating");
    }

    return;
  }

  bool wait_for_initialization = !camera_model_ || !calibration_parameters_;
  if (wait_for_initialization)
  {
    return;
  }

  if (!plane_calibration_)
  {
    plane_calibration_ = std::make_shared<PlaneCalibration>(*camera_model_, calibration_parameters_, depth_visualizer_);
  }

  if (!input_filter_)
  {
    input_filter_ = std::make_shared<InputFilter>(*camera_model_, calibration_parameters_, depth_visualizer_,
                                                  input_filter_config_);
  }

  if (!calibration_validation_)
  {
    calibration_validation_ = std::make_shared<CalibrationValidation>(*camera_model_, calibration_parameters_,
                                                                      calibration_validation_config_,
                                                                      depth_visualizer_);
  }

  if (!plane_to_depth_converter_)
  {
    plane_to_depth_converter_ = std::make_shared<PlaneToDepthImage>(camera_model_->getParameters());
  }

  Eigen::MatrixXf depth_matrix;

  bool converted_successfully = ImageMsgEigenConverter::convert(depth_image_msg, depth_matrix);
  if (!converted_successfully)
  {
    ROS_ERROR_STREAM("[PlaneCalibrationNodelet]: Conversion from image msg to Eigen matrix failed");
    return;
  }

  getTransform();

  if (!transform_)
  {
    return;
  }

  runCalibration(depth_matrix);
  publishTransform();
}

void PlaneCalibrationNodelet::getTransform()
{
  std::pair<Eigen::Vector3d, Eigen::AngleAxisd> transform;

  if (use_manual_ground_transform_)
  {
    transform = getTransformManual();
  }
  else
  {
    try
    {
      transform = getTransformTF();
    }
    catch (tf2::TransformException &ex)
    {
      ROS_WARN("%s", ex.what());
      return;
    }
  }

  bool transform_changed = true;
  if (transform_)
  {
    transform_changed = transform.first != transform_->first
        || transform.second.matrix() != transform_->second.matrix();
  }

  if (transform_changed)
  {
    ROS_INFO_STREAM("[PlaneCalibrationNodelet]: Sensor transform changed, resetting calibration");

    last_valid_calibration_result_plane_ = Eigen::MatrixXf();
    last_valid_calibration_result_ = std::make_pair(0.0, 0.0);
    last_valid_calibration_transformation_ = Eigen::Translation3d(transform.first) * transform.second;

    transform_ = std::make_shared<std::pair<Eigen::Vector3d, Eigen::AngleAxisd>>(transform);
    calibration_parameters_->update(transform_->first, transform_->second);
    input_filter_->updateBorders();
  }
}

std::pair<Eigen::Vector3d, Eigen::AngleAxisd> PlaneCalibrationNodelet::getTransformManual()
{
  Eigen::Vector3d offset = Eigen::Vector3d(x_offset_, y_offset_, z_offset_);
  Eigen::AngleAxisd rotation;
  rotation = Eigen::AngleAxisd(px_offset_, Eigen::Vector3d::UnitX())
      * Eigen::AngleAxisd(py_offset_, Eigen::Vector3d::UnitY())
      * Eigen::AngleAxisd(pz_offset_, Eigen::Vector3d::UnitZ());

  return std::make_pair(offset, rotation);
}

double restored_sensor_height(0.0f);
std::pair<Eigen::Vector3d, Eigen::AngleAxisd> PlaneCalibrationNodelet::getTransformTF()
{
  geometry_msgs::TransformStamped transformStamped;

  transformStamped = transform_listener_buffer_.lookupTransform(ground_frame_, camera_depth_frame_, ros::Time(0));

  Eigen::Affine3d eigen_transform;
  tf::transformMsgToEigen(transformStamped.transform, eigen_transform);

  Eigen::Vector3d sensor_z_axis = Eigen::Vector3d::UnitZ();
  double sensor_height = eigen_transform.translation()(2);
  
  restored_sensor_height = sensor_height;

  Eigen::Vector3d sensor_z_axis_in_footprint = eigen_transform.linear() * sensor_z_axis;
  double z_component = sensor_z_axis_in_footprint.z();
  double scale_to_ground = -sensor_height / z_component;

  Eigen::AngleAxisd rotation(eigen_transform.inverse().rotation());

  return std::make_pair(scale_to_ground * sensor_z_axis, rotation);
}

void PlaneCalibrationNodelet::runCalibration(Eigen::MatrixXf depth_matrix)
{
  if (debug_)
  {
    static tf2_ros::TransformBroadcaster transform_broadcaster;
    geometry_msgs::TransformStamped transformStamped;

    transformStamped.header.stamp = ros::Time::now();
    transformStamped.header.frame_id = camera_depth_frame_;

    CalibrationParameters::Parameters parameters = calibration_parameters_->getParameters();

    transformStamped.child_frame_id = "uncalibrated_ground";
    tf::transformEigenToMsg(parameters.getTransform(), transformStamped.transform);
    transform_broadcaster.sendTransform(transformStamped);

    depth_visualizer_->publishCloud("debug/uncalibrated_ground", parameters.getTransform(),
                                    camera_model_->getParameters());
  }

  Eigen::MatrixXf raw_depth = depth_matrix;
  
  // some candidate parameters
  const float maximum_range_of_depth_camera = 3.5f;
  const float threshold_normalized_z_by_xy = 0.13f;
  const float threshold_normalized_invalid_z_by_xy = 0.32f;
  const float max_angle_change = 0.8f;
  
  // some candidate parameters
  const float invalid_height_threshold = 0.04;
  
  // check plane here
  {
    CameraModel::Parameters cmp = camera_model_->getParameters();
    Eigen::Matrix3d rot = calibration_parameters_->getParameters().rotation_.matrix().transpose();
    
    
    std::vector< Eigen::Vector3f > xyzs( raw_depth.rows() * raw_depth.cols(), Eigen::Vector3f(0,0,0) );
    
    int idx(0);
    for( int i(0); i<raw_depth.rows(); i++ )
    {
      for( int j(0); j<raw_depth.cols(); j++, idx ++ )
      {
        double depth = raw_depth(i,j);
        if( std::isnan(depth) || depth > maximum_range_of_depth_camera )
        {
          continue;
        }
        
        Eigen::Vector3d nv(j,i,1);
        nv.x() -= cmp.center_x_;
        nv.y() -= cmp.center_y_;
        nv.x() /= cmp.f_x_;
        nv.y() /= cmp.f_y_;
        nv *= depth;
        
        xyzs[idx] = (rot * nv).cast<float>();
      }
    }
    
    const int sw = 2;
    const int erows = raw_depth.rows() - sw;
    const int ecols = raw_depth.cols() - sw;
    
    const float max_z_by_xy( std::tan(0.17453) );
    
    int valid_points(0);
    float avg_normalized_z_by_x(0);
    float avg_normalized_z_by_y(0);
    
    int invalid_points_x(0);
    int invalid_points_y(0);
    float avg_invalid_normalized_z_by_x(0);
    float avg_invalid_normalized_z_by_y(0);
     
    
    for( int i(sw); i<erows; i++ )
    {
      idx = i * raw_depth.cols() + sw;
      for( int j(sw); j<ecols; j++, idx++ )
      {
        if( xyzs[idx-sw].x() == 0.0f 
        || xyzs[idx+sw].x() == 0.0f
        || xyzs[idx+raw_depth.cols()*sw].x() == 0.0f
        || xyzs[idx-raw_depth.cols()*sw].x() == 0.0f )
        {
          continue;
        }
        
        Eigen::Vector3f dx = xyzs[idx+sw] - xyzs[idx-sw];
        Eigen::Vector3f dy = xyzs[idx+raw_depth.cols()*sw] - xyzs[idx-raw_depth.cols()*sw];
        
        valid_points++;
        
        float normalized_z_by_x = std::abs( dx.z() / dx.head<2>().norm() );
        float normalized_z_by_y = std::abs( dy.z() / dy.head<2>().norm() );   
        
        avg_normalized_z_by_x += normalized_z_by_x;
        avg_normalized_z_by_y += normalized_z_by_y;
        
        if( normalized_z_by_x > max_z_by_xy )
        {
          invalid_points_x++;
          avg_invalid_normalized_z_by_x += normalized_z_by_x;
        }
        
        if( normalized_z_by_y > max_z_by_xy )
        {
          invalid_points_y++;
          avg_invalid_normalized_z_by_y += normalized_z_by_y;          
        }
      }
    }
    
    if( valid_points < 40000 )
    {
      std::cout << "You do not have enough points for plane calibration " << std::endl;
      return;      
    }
    
    avg_normalized_z_by_x /= static_cast<float>( valid_points );
    avg_normalized_z_by_y /= static_cast<float>( valid_points );
    if( avg_normalized_z_by_x > threshold_normalized_z_by_xy 
      || avg_normalized_z_by_y > threshold_normalized_z_by_xy )
    {
      printf("valid_points = %d, normalized_z_by_x = %f, normalized_z_by_y = %f \n", valid_points, avg_normalized_z_by_x, avg_normalized_z_by_y );
      return;
    }
    
    if( invalid_points_x > 10 ) 
    {
      avg_invalid_normalized_z_by_x /= static_cast<float>( invalid_points_x );
      if( avg_invalid_normalized_z_by_x > threshold_normalized_invalid_z_by_xy )
      {
        printf("invalid_points_x = %d, avg_invalid_normalized_z_by_x = %f \n", invalid_points_x, avg_invalid_normalized_z_by_x );
        return;
      }
    }
    else 
    {
      printf("no invalid points along to x \n");
    }
    
    if( invalid_points_y ) 
    {
      avg_invalid_normalized_z_by_y /= static_cast<float>( invalid_points_y );
      if( avg_invalid_normalized_z_by_y > threshold_normalized_invalid_z_by_xy )
      {
        printf("invalid_points_y = %d, avg_invalid_normalized_z_by_y = %f \n", invalid_points_y, avg_invalid_normalized_z_by_y );
        return;
      }
    }
    else 
    {
      printf("no invalid points along to y \n");
    }
    
  }  
  
  input_filter_->filter(depth_matrix, debug_);

  bool input_data_not_usable = !input_filter_->dataIsUsable(depth_matrix, debug_);
  if (input_data_not_usable)
  {
    if (debug_)
    {
      ROS_WARN_STREAM("[PlaneCalibrationNodelet]: Input data not usable, not going to calibrate");
    }
    return;
  }

  if (!always_update_ && last_valid_calibration_result_plane_.size() != 0)
  {
    // is there any update from calibration module ?
    bool parameters_updated = calibration_parameters_->parametersUpdated();
    
    // can we use past calibration results for incoming data ?
    bool last_calibration_gone_bad = calibration_validation_->groundPlaneHasDataBelow( last_valid_calibration_result_plane_, depth_matrix, debug_);
    if (!parameters_updated && !last_calibration_gone_bad)
    {
      if (debug_)
      {
        ROS_INFO_STREAM("[PlaneCalibrationNodelet]: Last calibration data still works, not going to calibrate");
      }
      return;
    }
  }

  CalibrationParameters::Parameters parameters = calibration_parameters_->getParameters();
  std::pair<double, double> calibration_result = plane_calibration_->calibrate(depth_matrix, iterations_);

  if (debug_)
  {
    ROS_INFO_STREAM(
        "[PlaneCalibrationNodelet]: Calibration result angles [degree]: " << ecl::radians_to_degrees(calibration_result.first) << ", " << ecl::radians_to_degrees(calibration_result.second));

    Eigen::AngleAxisd rotation;
    rotation = parameters.rotation_ * Eigen::AngleAxisd(calibration_result.first, Eigen::Vector3d::UnitX())
        * Eigen::AngleAxisd(calibration_result.second, Eigen::Vector3d::UnitY());

    Eigen::Affine3d transform = Eigen::Translation3d(parameters.ground_plane_offset_) * rotation;

    depth_visualizer_->publishCloud("debug/calibration_result", transform, camera_model_->getParameters());
  }

  //  std::cout << "offset angles: " << ecl::radians_to_degrees(one_shot_result.first) << ", "
  //      << ecl::radians_to_degrees(one_shot_result.second) << std::endl;
  //  std::cout << "original angles: " << ecl::radians_to_degrees(px_offset_.load()) << ", "
  //      << ecl::radians_to_degrees(py_offset_.load()) << std::endl;

  bool valid_calibration_angles = calibration_validation_->angleOffsetValid(calibration_result);
  if (!valid_calibration_angles)
  {
    if (debug_)
    {
      ROS_WARN_STREAM(
          "[PlaneCalibrationNodelet]: Calibration turned out bad, too big angles ( > " << ecl::radians_to_degrees(parameters.max_deviation_) << " [degree]):");
      ROS_WARN_STREAM(
          "[PlaneCalibrationNodelet]: x,y angles [degree]: " << ecl::radians_to_degrees(calibration_result.first) << ", " << ecl::radians_to_degrees(calibration_result.second));
    }
    //keep old / last one
    return;
  }
  
  // check discontinuity
  double xangle_diff = ecl::radians_to_degrees( std::abs( calibration_result.first - last_calibration_result_.first ) );
  double yangle_diff = ecl::radians_to_degrees( std::abs( calibration_result.second - last_calibration_result_.second ) );
  
  last_calibration_result_ = calibration_result;
  if( xangle_diff > max_angle_change || yangle_diff > max_angle_change )
  {
    if (debug_)
    {
      ROS_WARN_STREAM( "[PlaneCalibrationNodelet]: Too big change of angles. xangle_diff[degree]: " << xangle_diff << ", yangle_diff[degree] " << yangle_diff );
    }
  }
  
  

  Eigen::AngleAxisd rotation;
  rotation = parameters.rotation_ * Eigen::AngleAxisd(calibration_result.first, Eigen::Vector3d::UnitX())
      * Eigen::AngleAxisd(calibration_result.second, Eigen::Vector3d::UnitY());

  Eigen::Affine3d transform = Eigen::Translation3d(parameters.ground_plane_offset_) * rotation;
  Eigen::MatrixXf new_ground_plane = plane_to_depth_converter_->convert(transform);
  
  {
    CameraModel::Parameters cmp = camera_model_->getParameters();
    Eigen::Matrix3d rot = rotation.matrix();
    
    float avg_height(0);
    float avg_invalid_point_height(0);
    int valid_points(0);
    int invalid_points(0);
    
    for( int i(0); i<raw_depth.rows(); i++ )
    {
      for( int j(0); j<raw_depth.cols(); j++ )
      {
        double depth = raw_depth(i,j);
        if( std::isnan(depth) || depth > maximum_range_of_depth_camera )
        {
          continue;
        }
        
        Eigen::Vector3d nv(j,i,1);
        nv.x() -= cmp.center_x_;
        nv.y() -= cmp.center_y_;
        nv.x() /= cmp.f_x_;
        nv.y() /= cmp.f_y_;
        nv *= depth;
        
        Eigen::Vector3d prj = rot.transpose() * nv;
        double height_from_ground = prj.z() + restored_sensor_height;
        
        avg_height += std::abs(height_from_ground);
        valid_points ++;
        if( std::abs(height_from_ground) > invalid_height_threshold )
        {
          avg_invalid_point_height += std::abs(height_from_ground);
          invalid_points ++;
        }
      }
    }

    if( invalid_points > 10 )
    {
      printf("invalid points = %d, avg_height_of_invalid_points = %lf \n", invalid_points, avg_invalid_point_height/(invalid_points) ); 
      if (debug_)
      {
        ROS_WARN_STREAM("[PlaneCalibrationNodelet]: There would be taller obstacles");
      }      
      return;
    }
    else 
    {
      printf("valid points = %d, avg_height = %lf \n", valid_points, avg_height/valid_points );
    }
  }

  bool good_calibration = calibration_validation_->groundPlaneFitsData(new_ground_plane, depth_matrix, debug_);
  if (!good_calibration)
  {
    if (debug_)
    {
      ROS_WARN_STREAM("[PlaneCalibrationNodelet]: Calibration turned out bad: Data does not fit good enough");
    }
    //keep old / last one
    return;
  }

  std::stringstream angle_change_string;
  angle_change_string << "px [degree]: " << ecl::radians_to_degrees(last_valid_calibration_result_.first) << " -> "
      << ecl::radians_to_degrees(calibration_result.first);
  angle_change_string << ", ";
  angle_change_string << "py [degree]: " << ecl::radians_to_degrees(last_valid_calibration_result_.second) << " -> "
      << ecl::radians_to_degrees(calibration_result.second);

  geometry_msgs::Pose2D update_msg;
  update_msg.x = ecl::radians_to_degrees(last_valid_calibration_result_.first);
  update_msg.y = ecl::radians_to_degrees(last_valid_calibration_result_.second);
  update_msg.theta = 0.0;
  pub_update_.publish(update_msg);

  ROS_INFO_STREAM("[PlaneCalibrationNodelet]: Updated the calibration angles: " << angle_change_string.str());

  last_valid_calibration_result_ = calibration_result;
  last_valid_calibration_result_plane_ = new_ground_plane;
  last_valid_calibration_transformation_ = transform;
}

void PlaneCalibrationNodelet::publishTransform()
{
  static tf2_ros::TransformBroadcaster transform_broadcaster;
  geometry_msgs::TransformStamped transformStamped;

  transformStamped.header.stamp = ros::Time::now();
  transformStamped.header.frame_id = camera_depth_frame_;
  transformStamped.child_frame_id = result_frame_;

  if (transform_)
  {
    Eigen::Affine3d inverse_transform = (Eigen::Translation3d(transform_->first) * transform_->second).inverse();

    Eigen::Affine3d camera_from_detected_ground = last_valid_calibration_transformation_ * inverse_transform;
    tf::transformEigenToMsg(camera_from_detected_ground.inverse(), transformStamped.transform);
  }
  else
  {
    //publish same if no transform available yet
    Eigen::Affine3d no_transform = Eigen::Translation3d(0.0, 0.0, 0.0) * Eigen::AngleAxisd::Identity();
    tf::transformEigenToMsg(no_transform, transformStamped.transform);
  }

  transform_broadcaster.sendTransform(transformStamped);

  if (debug_)
  {
    transformStamped.child_frame_id = "detected_ground";
    tf::transformEigenToMsg(last_valid_calibration_transformation_, transformStamped.transform);
    transform_broadcaster.sendTransform(transformStamped);

    depth_visualizer_->publishCloud("debug/calibrated_plane", last_valid_calibration_transformation_,
                                    camera_model_->getParameters());
  }
}

} /* end namespace */

PLUGINLIB_EXPORT_CLASS(plane_calibration::PlaneCalibrationNodelet, nodelet::Nodelet)
