// Copyright (c) 2008, Willow Garage, Inc.
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//  * Neither the name of the Willow Garage nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "cv_bridge/cv_bridge.hpp"

#include <image_transport/camera_common.hpp>
#include <image_transport/image_transport.hpp>
#include <image_transport/subscriber_filter.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <depth_image_proc/conversions.hpp>
#include <depth_image_proc/point_cloud_xyzi.hpp>
#include <opencv2/imgproc/imgproc.hpp>

namespace depth_image_proc
{

PointCloudXyziNode::PointCloudXyziNode(const rclcpp::NodeOptions & options)
: rclcpp::Node("PointCloudXyziNode", options)
{
  // TransportHints does not actually declare the parameter
  this->declare_parameter<std::string>("image_transport", "raw");
  this->declare_parameter<std::string>("depth_image_transport", "raw");

  // value used for invalid points for pcd conversion
  invalid_depth_ = this->declare_parameter<double>("invalid_depth", 0.0);

  // Read parameters
  int queue_size = this->declare_parameter<int>("queue_size", 5);

  // Synchronize inputs. Topic subscriptions happen on demand in the connection callback.
  sync_ = std::make_shared<Synchronizer>(
    SyncPolicy(queue_size),
    sub_depth_,
    sub_intensity_,
    sub_info_);
  sync_->registerCallback(
    std::bind(
      &PointCloudXyziNode::imageCb,
      this,
      std::placeholders::_1,
      std::placeholders::_2,
      std::placeholders::_3));

  // Create publisher with connect callback
  rclcpp::PublisherOptions pub_options;
  pub_options.event_callbacks.matched_callback =
    [this](rclcpp::MatchedInfo & s)
    {
      std::lock_guard<std::mutex> lock(connect_mutex_);
      if (s.current_count == 0) {
        sub_depth_.unsubscribe();
        sub_intensity_.unsubscribe();
        sub_info_.unsubscribe();
      } else if (!sub_depth_.getSubscriber()) {
        // For compressed topics to remap appropriately, we need to pass a
        // fully expanded and remapped topic name to image_transport
        auto node_base = this->get_node_base_interface();
        std::string depth_topic =
          node_base->resolve_topic_or_service_name("depth/image_rect", false);
        std::string intensity_topic =
          node_base->resolve_topic_or_service_name("intensity/image_rect", false);
        // Allow also remapping camera_info to something different than default
        std::string intensity_info_topic =
          node_base->resolve_topic_or_service_name(
          image_transport::getCameraInfoTopic(intensity_topic), false);

        // depth image can use different transport.(e.g. compressedDepth)
        image_transport::TransportHints depth_hints(this, "raw", "depth_image_transport");
        sub_depth_.subscribe(this, depth_topic, depth_hints.getTransport());

        // intensity uses normal ros transport hints.
        image_transport::TransportHints hints(this, "raw");
        sub_intensity_.subscribe(this, intensity_topic, hints.getTransport());
        sub_info_.subscribe(this, intensity_info_topic, rclcpp::QoS(10));
      }
    };
  pub_point_cloud_ = create_publisher<PointCloud>("points", rclcpp::SensorDataQoS(), pub_options);
}

void PointCloudXyziNode::imageCb(
  const sensor_msgs::msg::Image::ConstSharedPtr & depth_msg,
  const sensor_msgs::msg::Image::ConstSharedPtr & intensity_msg_in,
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr & info_msg)
{
  // Check for bad inputs
  if (depth_msg->header.frame_id != intensity_msg_in->header.frame_id) {
    RCLCPP_WARN_THROTTLE(
      get_logger(),
      *get_clock(),
      10000,  // 10 seconds
      "Depth image frame id [%s] doesn't match image frame id [%s]",
      depth_msg->header.frame_id.c_str(), intensity_msg_in->header.frame_id.c_str());
  }

  // Update camera model
  model_.fromCameraInfo(info_msg);

  // Check if the input image has to be resized
  sensor_msgs::msg::Image::ConstSharedPtr intensity_msg = intensity_msg_in;
  if (depth_msg->width != intensity_msg->width || depth_msg->height != intensity_msg->height) {
    sensor_msgs::msg::CameraInfo info_msg_tmp = *info_msg;
    info_msg_tmp.width = depth_msg->width;
    info_msg_tmp.height = depth_msg->height;
    float ratio = static_cast<float>(depth_msg->width) / static_cast<float>(intensity_msg->width);
    info_msg_tmp.k[0] *= ratio;
    info_msg_tmp.k[2] *= ratio;
    info_msg_tmp.k[4] *= ratio;
    info_msg_tmp.k[5] *= ratio;
    info_msg_tmp.p[0] *= ratio;
    info_msg_tmp.p[2] *= ratio;
    info_msg_tmp.p[5] *= ratio;
    info_msg_tmp.p[6] *= ratio;
    model_.fromCameraInfo(info_msg_tmp);

    cv_bridge::CvImageConstPtr cv_ptr;
    try {
      cv_ptr = cv_bridge::toCvShare(intensity_msg, intensity_msg->encoding);
    } catch (const cv_bridge::Exception & e) {
      RCLCPP_ERROR(get_logger(), "cv_bridge exception: %s", e.what());
      return;
    }
    cv_bridge::CvImage cv_rsz;
    cv_rsz.header = cv_ptr->header;
    cv_rsz.encoding = cv_ptr->encoding;
    cv::resize(
      cv_ptr->image.rowRange(0, depth_msg->height / ratio), cv_rsz.image,
      cv::Size(depth_msg->width, depth_msg->height));
    if ((intensity_msg->encoding == enc::MONO8) || (intensity_msg->encoding == enc::MONO16)) {
      intensity_msg = cv_rsz.toImageMsg();
    } else {
      intensity_msg = cv_bridge::toCvCopy(cv_rsz.toImageMsg(), enc::MONO8)->toImageMsg();
    }

    // RCLCPP_ERROR(get_logger(), "Depth resolution (%ux%u) does not match resolution (%ux%u)",
    //              depth_msg->width, depth_msg->height, rgb_msg->width, rgb_msg->height);
    // return;
  } else {
    intensity_msg = intensity_msg_in;
  }

  // Supported color encodings: MONO8, MONO16
  if (intensity_msg->encoding != enc::MONO8 || intensity_msg->encoding != enc::MONO16) {
    try {
      intensity_msg = cv_bridge::toCvCopy(intensity_msg, enc::MONO8)->toImageMsg();
    } catch (const cv_bridge::Exception & e) {
      RCLCPP_ERROR(
        get_logger(), "Unsupported encoding [%s]: %s",
        intensity_msg->encoding.c_str(), e.what());
      return;
    }
  }

  auto cloud_msg = std::make_shared<PointCloud>();
  cloud_msg->header = depth_msg->header;  // Use depth image time stamp
  cloud_msg->height = depth_msg->height;
  cloud_msg->width = depth_msg->width;
  cloud_msg->is_dense = false;
  cloud_msg->is_bigendian = false;

  sensor_msgs::PointCloud2Modifier pcd_modifier(*cloud_msg);
  // pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "i");
  pcd_modifier.setPointCloud2Fields(
    4,
    "x", 1, sensor_msgs::msg::PointField::FLOAT32,
    "y", 1, sensor_msgs::msg::PointField::FLOAT32,
    "z", 1, sensor_msgs::msg::PointField::FLOAT32,
    "intensity", 1, sensor_msgs::msg::PointField::FLOAT32);

  // Convert Depth Image to Pointcloud
  if (depth_msg->encoding == enc::TYPE_16UC1) {
    convertDepth<uint16_t>(depth_msg, cloud_msg, model_, invalid_depth_);
  } else if (depth_msg->encoding == enc::TYPE_32FC1) {
    convertDepth<float>(depth_msg, cloud_msg, model_, invalid_depth_);
  } else {
    RCLCPP_ERROR(
      get_logger(), "Depth image has unsupported encoding [%s]", depth_msg->encoding.c_str());
    return;
  }

  // Convert Intensity Image to Pointcloud
  if (intensity_msg->encoding == enc::MONO8) {
    convertIntensity<uint8_t>(intensity_msg, cloud_msg);
  } else if (intensity_msg->encoding == enc::MONO16) {
    convertIntensity<uint16_t>(intensity_msg, cloud_msg);
  } else if (intensity_msg->encoding == enc::TYPE_16UC1) {
    convertIntensity<uint16_t>(intensity_msg, cloud_msg);
  } else if (intensity_msg->encoding == enc::TYPE_32FC1) {
    convertIntensity<float>(intensity_msg, cloud_msg);
  } else {
    RCLCPP_ERROR(
      get_logger(), "Intensity image has unsupported encoding [%s]",
      intensity_msg->encoding.c_str());
    return;
  }

  pub_point_cloud_->publish(*cloud_msg);
}


}  // namespace depth_image_proc

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
RCLCPP_COMPONENTS_REGISTER_NODE(depth_image_proc::PointCloudXyziNode)
