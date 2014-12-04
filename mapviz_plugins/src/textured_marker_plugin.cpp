// *****************************************************************************
//
// Copyright (C) 2014 All Right Reserved, Southwest Research Institute® (SwRI®)
//
// Contract No.  10-58058A
// Contractor    Southwest Research Institute® (SwRI®)
// Address       6220 Culebra Road, San Antonio, Texas 78228-0510
// Contact       Steve Dellenback <sdellenback@swri.org> (210) 522-3914
//
// This code was developed as part of an internal research project fully funded
// by Southwest Research Institute®.
//
// THIS CODE AND INFORMATION ARE PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
// KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// *****************************************************************************

#include <mapviz_plugins/textured_marker_plugin.h>

// C++ standard libraries
#include <cmath>
#include <cstdio>
#include <vector>

// Boost libraries
#include <boost/algorithm/string.hpp>

// QT libraries
#include <QDialog>
#include <QGLWidget>

// ROS libraries
#include <ros/master.h>
#include <sensor_msgs/image_encodings.h>

#include <math_util/constants.h>
#include <yaml_util/yaml_util.h>

// Declare plugin
#include <pluginlib/class_list_macros.h>
PLUGINLIB_DECLARE_CLASS(
    mapviz_plugins,
    textured_marker,
    mapviz_plugins::TexturedMarkerPlugin,
    mapviz::MapvizPlugin)

namespace mapviz_plugins
{
  TexturedMarkerPlugin::TexturedMarkerPlugin() :
    config_widget_(new QWidget()),
    is_marker_array_(false)
  {
    ui_.setupUi(config_widget_);

    // Set background white
    QPalette p(config_widget_->palette());
    p.setColor(QPalette::Background, Qt::white);
    config_widget_->setPalette(p);

    // Set status text red
    QPalette p3(ui_.status->palette());
    p3.setColor(QPalette::Text, Qt::red);
    ui_.status->setPalette(p3);

    QObject::connect(ui_.selecttopic, SIGNAL(clicked()), this, SLOT(SelectTopic()));
    QObject::connect(ui_.topic, SIGNAL(editingFinished()), this, SLOT(TopicEdited()));
  }

  TexturedMarkerPlugin::~TexturedMarkerPlugin()
  {
  }

  void TexturedMarkerPlugin::SelectTopic()
  {
    QDialog dialog;
    Ui::topicselect ui;
    ui.setupUi(&dialog);

    std::vector<ros::master::TopicInfo> topics;
    ros::master::getTopics(topics);

    for (unsigned int i = 0; i < topics.size(); i++)
    {
      if (topics[i].datatype == "marti_visualization_msgs/TexturedMarker" || topics[i].datatype == "marti_visualization_msgs/TexturedMarkerArray")
      {
        ui.displaylist->addItem(topics[i].name.c_str());
      }
    }
    ui.displaylist->setCurrentRow(0);

    dialog.exec();

    if (dialog.result() == QDialog::Accepted && ui.displaylist->selectedItems().count() == 1)
    {
      ui_.topic->setText(ui.displaylist->selectedItems().first()->text());

      // Determine if this is a marker array
      is_marker_array_ = false;
      for (unsigned int i = 0; i < topics.size(); i++)
      {
        if (topics[i].datatype == "marti_visualization_msgs/TexturedMarkerArray")
        {
          if (ui.displaylist->selectedItems().first()->text().toStdString() == topics[i].name)
          {
            is_marker_array_ = true;
          }
        }
      }

      TopicEdited();
    }
  }

  void TexturedMarkerPlugin::TopicEdited()
  {
    if (ui_.topic->text().toStdString() != topic_)
    {
      initialized_ = false;
      markers_.clear();
      has_message_ = false;
      topic_ = boost::trim_copy(ui_.topic->text().toStdString());
      PrintWarning("No messages received.");

      marker_sub_.shutdown();

      if (is_marker_array_)
      {
        marker_sub_ = node_.subscribe(topic_, 1000, &TexturedMarkerPlugin::MarkerArrayCallback, this);
      }
      else
      {
        marker_sub_ = node_.subscribe(topic_, 1000, &TexturedMarkerPlugin::MarkerCallback, this);
      }

      ROS_INFO("Subscribing to %s", topic_.c_str());
    }
  }

  void TexturedMarkerPlugin::ProcessMarker(const marti_visualization_msgs::TexturedMarker& marker)
  {
    if (!has_message_)
    {
      source_frame_ = marker.header.frame_id;
      initialized_ = true;
      has_message_ = true;
    }

    if (marker.action == marti_visualization_msgs::TexturedMarker::ADD)
    {
      MarkerData& markerData = markers_[marker.ns][marker.id];
      markerData.stamp = marker.header.stamp;

      markerData.transformed = true;
      markerData.alpha_ = marker.alpha;

      transform_util::Transform transform;
      if (!GetTransform(marker.header.stamp, transform))
      {
        markerData.transformed = false;
        PrintError("No transform between " + source_frame_ + " and " + target_frame_);
      }

      // Handle lifetime parameter
      ros::Duration lifetime = marker.lifetime;
      if (lifetime.isZero())
      {
        markerData.expire_time = ros::TIME_MAX;
      }
      else
      {
        // Temporarily add 5 seconds to fix some existing markers.
        markerData.expire_time = ros::Time::now() + lifetime + ros::Duration(5);
      }

      tf::Transform offset(
        tf::Quaternion(
          marker.pose.orientation.x,
          marker.pose.orientation.y,
          marker.pose.orientation.z,
          marker.pose.orientation.w), 
        tf::Vector3(
          marker.pose.position.x,
          marker.pose.position.y,
          marker.pose.position.z));

      double right = marker.image.width * marker.resolution / 2.0;
      double left = -right;
      double top = marker.image.height * marker.resolution / 2.0;
      double bottom = -top;

      tf::Vector3 top_left(left, top, 0);
      tf::Vector3 top_right(right, top, 0);
      tf::Vector3 bottom_left(left, bottom, 0);
      tf::Vector3 bottom_right(right, bottom, 0);
      
      top_left = offset * top_left;
      top_right = offset * top_right;
      bottom_left = offset * bottom_left;
      bottom_right = offset * bottom_right;
      
      markerData.quad_.clear();
      markerData.quad_.push_back(top_left);
      markerData.quad_.push_back(top_right);
      markerData.quad_.push_back(bottom_right);
      
      markerData.quad_.push_back(top_left);
      markerData.quad_.push_back(bottom_right);
      markerData.quad_.push_back(bottom_left);
      
      markerData.transformed_quad_.clear();
      for (size_t i = 0; i < markerData.quad_.size(); i++)
      {
        markerData.transformed_quad_.push_back(transform * markerData.quad_[i]);
      }
      
      uint32_t max_dimension = std::max(marker.image.height, marker.image.width);
      uint32_t new_size = 1;
      while (new_size < max_dimension)
        new_size = new_size << 1;
      
      if (new_size != markerData.texture_size_ || markerData.encoding_ != marker.image.encoding)
      {
        markerData.texture_size_ = new_size;
        
        markerData.encoding_ = marker.image.encoding;
        
        GLuint ids[1];

        //  Free the current texture.
        if (markerData.texture_id_ != -1)
        {
          ids[0] = markerData.texture_id_;
          glDeleteTextures(1, &ids[0]);
        }
        
        // Get a new texture id.
        glGenTextures(1, &ids[0]);
        markerData.texture_id_ = ids[0];

        // Bind the texture with the correct size and null memory.
        glBindTexture(GL_TEXTURE_2D, markerData.texture_id_);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        if (markerData.encoding_ == sensor_msgs::image_encodings::BGRA8)
        {
          markerData.texture_.resize(markerData.texture_size_ * markerData.texture_size_ * 4);
        }
        else if (markerData.encoding_ == sensor_msgs::image_encodings::BGR8)
        {
          markerData.texture_.resize(markerData.texture_size_ * markerData.texture_size_ * 3);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

        glBindTexture(GL_TEXTURE_2D, 0);
      }
      
      glBindTexture(GL_TEXTURE_2D, markerData.texture_id_);
      
      if (markerData.encoding_ == sensor_msgs::image_encodings::BGRA8)
      {
        for (size_t row = 0; row < marker.image.height; row++)
        {
          for (size_t col = 0; col < marker.image.width; col++)
          {
            size_t src_index = (row * marker.image.width + col) * 4;
            size_t dst_index = (row * markerData.texture_size_ + col) * 4;
            
            markerData.texture_[dst_index + 0] = marker.image.data[src_index + 0];
            markerData.texture_[dst_index + 1] = marker.image.data[src_index + 1];
            markerData.texture_[dst_index + 2] = marker.image.data[src_index + 2];
            markerData.texture_[dst_index + 3] = marker.image.data[src_index + 3];
          }
        }
      
        glTexImage2D(
            GL_TEXTURE_2D, 
            0, 
            GL_RGBA, 
            markerData.texture_size_, 
            markerData.texture_size_, 
            0, 
            GL_BGRA, 
            GL_UNSIGNED_BYTE, 
            markerData.texture_.data());
      }
      else if (markerData.encoding_ == sensor_msgs::image_encodings::BGR8)
      {
        for (size_t row = 0; row < marker.image.height; row++)
        {
          for (size_t col = 0; col < marker.image.width; col++)
          {
            size_t src_index = (row * marker.image.width + col) * 3;
            size_t dst_index = (row * markerData.texture_size_ + col) * 3;
            
            markerData.texture_[dst_index + 0] = marker.image.data[src_index + 0];
            markerData.texture_[dst_index + 1] = marker.image.data[src_index + 1];
            markerData.texture_[dst_index + 2] = marker.image.data[src_index + 2];
          }
        }
      
        glTexImage2D(
            GL_TEXTURE_2D, 
            0, 
            GL_RGB, 
            markerData.texture_size_, 
            markerData.texture_size_, 
            0, 
            GL_BGR, 
            GL_UNSIGNED_BYTE, 
            markerData.texture_.data());
      }
      
      glBindTexture(GL_TEXTURE_2D, 0);
      
      markerData.texture_x_ = static_cast<float>(marker.image.width) / static_cast<float>(markerData.texture_size_);
      markerData.texture_y_ = static_cast<float>(marker.image.height) / static_cast<float>(markerData.texture_size_);
    }
    else
    {
      markers_[marker.ns].erase(marker.id);
    }

    canvas_->update();
  }

  void TexturedMarkerPlugin::MarkerCallback(const marti_visualization_msgs::TexturedMarkerConstPtr marker)
  {
    ProcessMarker(*marker);
  }

  void TexturedMarkerPlugin::MarkerArrayCallback(const marti_visualization_msgs::TexturedMarkerArrayConstPtr markers)
  {
    for (unsigned int i = 0; i < markers->markers.size(); i++)
    {
      ProcessMarker(markers->markers[i]);
    }
  }

  void TexturedMarkerPlugin::PrintError(const std::string& message)
  {
    if (message == ui_.status->text().toStdString())
      return;

    ROS_ERROR("Error: %s", message.c_str());
    QPalette p(ui_.status->palette());
    p.setColor(QPalette::Text, Qt::red);
    ui_.status->setPalette(p);
    ui_.status->setText(message.c_str());
  }

  void TexturedMarkerPlugin::PrintInfo(const std::string& message)
  {
    if (message == ui_.status->text().toStdString())
      return;

    ROS_INFO("%s", message.c_str());
    QPalette p(ui_.status->palette());
    p.setColor(QPalette::Text, Qt::green);
    ui_.status->setPalette(p);
    ui_.status->setText(message.c_str());
  }

  void TexturedMarkerPlugin::PrintWarning(const std::string& message)
  {
    if (message == ui_.status->text().toStdString())
      return;

    ROS_WARN("%s", message.c_str());
    QPalette p(ui_.status->palette());
    p.setColor(QPalette::Text, Qt::darkYellow);
    ui_.status->setPalette(p);
    ui_.status->setText(message.c_str());
  }

  QWidget* TexturedMarkerPlugin::GetConfigWidget(QWidget* parent)
  {
    config_widget_->setParent(parent);

    return config_widget_;
  }

  bool TexturedMarkerPlugin::Initialize(QGLWidget* canvas)
  {
    canvas_ = canvas;

    return true;
  }

  void TexturedMarkerPlugin::Draw(double x, double y, double scale)
  {
    ros::Time now = ros::Time::now();

    std::map<std::string, std::map<int, MarkerData> >::iterator nsIter;
    for (nsIter = markers_.begin(); nsIter != markers_.end(); ++nsIter)
    {
      std::map<int, MarkerData>::iterator markerIter;
      for (markerIter = nsIter->second.begin(); markerIter != nsIter->second.end(); ++markerIter)
      {
        MarkerData& marker = markerIter->second;

        if (marker.expire_time > now)
        {
          if (marker.transformed)
          {
            glEnable(GL_TEXTURE_2D);

            glBindTexture(GL_TEXTURE_2D, marker.texture_id_);
          
            glBegin(GL_TRIANGLES);
            
            glColor4f(1.0f, 1.0f, 1.0f, marker.alpha_);

            double x = marker.texture_x_;
            double y = marker.texture_y_;

            glTexCoord2f(0, 0); glVertex2d(marker.transformed_quad_[0].x(), marker.transformed_quad_[0].y());
            glTexCoord2f(x, 0); glVertex2d(marker.transformed_quad_[1].x(), marker.transformed_quad_[1].y());
            glTexCoord2f(x, y); glVertex2d(marker.transformed_quad_[2].x(), marker.transformed_quad_[2].y());

            glTexCoord2f(0, 0); glVertex2d(marker.transformed_quad_[3].x(), marker.transformed_quad_[3].y());
            glTexCoord2f(x, y); glVertex2d(marker.transformed_quad_[4].x(), marker.transformed_quad_[4].y());
            glTexCoord2f(0, y); glVertex2d(marker.transformed_quad_[5].x(), marker.transformed_quad_[5].y());

            glEnd();
            
            glBindTexture(GL_TEXTURE_2D, 0);

            glDisable(GL_TEXTURE_2D);
            
            PrintInfo("OK");
          }
        }
      }
    }
  }

  void TexturedMarkerPlugin::Transform()
  {  
    std::map<std::string, std::map<int, MarkerData> >::iterator nsIter;
    for (nsIter = markers_.begin(); nsIter != markers_.end(); ++nsIter)
    {
      std::map<int, MarkerData>::iterator markerIter;
      for (markerIter = nsIter->second.begin(); markerIter != nsIter->second.end(); ++markerIter)
      {
        transform_util::Transform transform;
        if (GetTransform(markerIter->second.stamp, transform))
        {
          markerIter->second.transformed_quad_.clear();
          for (size_t i = 0; i < markerIter->second.quad_.size(); i++)
          {
            markerIter->second.transformed_quad_.push_back(transform * markerIter->second.quad_[i]);
          }
        }
      }
    }
  }

  void TexturedMarkerPlugin::LoadConfig(const YAML::Node& node, const std::string& path)
  {
    std::string topic;
    node["topic"] >> topic;
    ui_.topic->setText(boost::trim_copy(topic).c_str());

    node["is_marker_array"] >> is_marker_array_;

    TopicEdited();
  }

  void TexturedMarkerPlugin::SaveConfig(YAML::Emitter& emitter, const std::string& path)
  {
    emitter << YAML::Key << "topic" << YAML::Value << boost::trim_copy(ui_.topic->text().toStdString());
    emitter << YAML::Key << "is_marker_array" << YAML::Value << is_marker_array_;
  }
}

