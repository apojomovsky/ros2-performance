/* Software License Agreement (BSD License)
 *
 *  Copyright (c) 2019, iRobot ROS
 *  All rights reserved.
 *
 *  This file is part of ros2-performance, which is released under BSD-3-Clause.
 *  You may use, distribute and modify this code under the BSD-3-Clause license.
 */

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <tuple>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/qos.hpp"

#include "performance_test/communication.hpp"
#include "performance_test/events_logger.hpp"
#include "performance_test/performance_node_base.hpp"
#include "performance_test/tracker.hpp"

using namespace std::chrono_literals;

namespace performance_test
{

PerformanceNodeBase::PerformanceNodeBase(int executor_id)
{
  m_executor_id = executor_id;
}

rclcpp::node_interfaces::NodeBaseInterface::SharedPtr
PerformanceNodeBase::get_node_base()
{
  return m_node_base;
}

rclcpp::node_interfaces::NodeGraphInterface::SharedPtr
PerformanceNodeBase::get_node_graph()
{
  return m_node_graph;
}

rclcpp::Logger
PerformanceNodeBase::get_node_logger()
{
  return m_node_logging->get_logger();
}

const char *
PerformanceNodeBase::get_node_name()
{
  return m_node_base->get_name();
}

void PerformanceNodeBase::add_timer(
  std::chrono::microseconds period,
  std::function<void()> callback)
{
  rclcpp::TimerBase::SharedPtr timer = rclcpp::create_wall_timer(
    period,
    callback,
    nullptr,
    m_node_base.get(),
    m_node_timers.get());

  _timers.push_back(timer);
}

std::shared_ptr<PerformanceNodeBase::Trackers> PerformanceNodeBase::sub_and_client_trackers()
{
  auto trackers = std::make_shared<PerformanceNodeBase::Trackers>();
  for(const auto & sub : _subs)
  {
    trackers->push_back({sub.first, sub.second.second});
  }

  for(const auto & client : _clients)
  {
    trackers->push_back({client.first, std::get<1>(client.second)});
  }

  return trackers;
}

std::shared_ptr<PerformanceNodeBase::Trackers> PerformanceNodeBase::pub_trackers()
{
  auto trackers = std::make_shared<PerformanceNodeBase::Trackers>();

  for(const auto & pub : _pubs)
  {
    trackers->push_back({pub.first, pub.second.second});
  }

  return trackers;
}

void PerformanceNodeBase::set_events_logger(std::shared_ptr<EventsLogger> ev)
{
  assert(ev != nullptr && "Called `PerformanceNode::set_events_logger` passing a nullptr!");

  _events_logger = ev;
}

int PerformanceNodeBase::get_executor_id()
{
  return m_executor_id;
}

std::vector<std::string> PerformanceNodeBase::get_published_topics()
{
  std::vector<std::string> topics;

  for (const auto & pub_tracker : _pubs) {
    std::string topic_name = pub_tracker.first;
    topics.push_back(topic_name);
  }

  return topics;
}

void PerformanceNodeBase::store_subscription(
  rclcpp::SubscriptionBase::SharedPtr sub,
  const std::string & topic_name,
  const Tracker::TrackingOptions & tracking_options)
{
  _subs.insert({ topic_name, { sub, Tracker(m_node_base->get_name(), topic_name, tracking_options) } });
  RCLCPP_INFO(m_node_logging->get_logger(), "Subscriber to %s created", topic_name.c_str());
}

void PerformanceNodeBase::store_publisher(
  rclcpp::PublisherBase::SharedPtr pub,
  const std::string & topic_name,
  const Tracker::TrackingOptions & tracking_options)
{
  _pubs.insert({ topic_name, { pub, Tracker(m_node_base->get_name(), topic_name, tracking_options) } });
  RCLCPP_INFO(m_node_logging->get_logger(), "Publisher to %s created", topic_name.c_str());
}

void PerformanceNodeBase::store_client(
  rclcpp::ClientBase::SharedPtr client,
  const std::string & service_name,
  const Tracker::TrackingOptions & tracking_options)
{
  _clients.insert(
    {
      service_name,
      std::tuple<std::shared_ptr<rclcpp::ClientBase>, Tracker, Tracker::TrackingNumber>{
        client,
        Tracker(m_node_base->get_name(), service_name, tracking_options),
        0
      }
    });

  RCLCPP_INFO(m_node_logging->get_logger(), "Client to %s created", service_name.c_str());
}

void PerformanceNodeBase::store_server(
  rclcpp::ServiceBase::SharedPtr server,
  const std::string & service_name,
  const Tracker::TrackingOptions & tracking_options)
{
  _servers.insert({ service_name, { server, Tracker(m_node_base->get_name(), service_name, tracking_options) } });
  RCLCPP_INFO(m_node_logging->get_logger(), "Server to %s created", service_name.c_str());
}

performance_test_msgs::msg::PerformanceHeader
PerformanceNodeBase::create_msg_header(
  rclcpp::Time publish_time,
  float pub_frequency,
  Tracker::TrackingNumber tracking_number,
  size_t msg_size)
{
  performance_test_msgs::msg::PerformanceHeader header;

  header.size = msg_size;
  // get the frequency value that we stored when creating the publisher
  header.frequency = pub_frequency;
  // set the tracking count for this message
  header.tracking_number = tracking_number;
  // attach the timestamp as last operation before publishing
  header.stamp = publish_time;

  return header;
}

void PerformanceNodeBase::_handle_sub_received_msg(
  const std::string & topic_name,
  const performance_test_msgs::msg::PerformanceHeader & msg_header)
{
  // Scan new message's header
  auto & tracker = _subs.at(topic_name).second;
  tracker.scan(msg_header, m_node_clock->get_clock()->now(), _events_logger);

  RCLCPP_DEBUG(m_node_logging->get_logger(), "Received on %s msg number %d after %lu us", topic_name.c_str(), msg_header.tracking_number, tracker.last());
}

void PerformanceNodeBase::_handle_client_received_response(
  const std::string & service_name,
  const performance_test_msgs::msg::PerformanceHeader & request_header,
  const performance_test_msgs::msg::PerformanceHeader & response_header)
{
  (void)response_header;

  auto & tracker = std::get<1>(_clients.at(service_name));
  tracker.scan(request_header, m_node_clock->get_clock()->now(), _events_logger);

  RCLCPP_DEBUG(m_node_logging->get_logger(), "Response on %s request number %d received after %lu us", service_name.c_str(), request_header.tracking_number, tracker.last());
}

performance_test_msgs::msg::PerformanceHeader
PerformanceNodeBase::_handle_server_received_request(
  const std::string & service_name,
  const performance_test_msgs::msg::PerformanceHeader & request_header)
{
  // we use the tracker to store some information also on the server side
  auto & tracker = _servers.at(service_name).second;

  auto response_header = this->create_msg_header(
    m_node_clock->get_clock()->now(),
    request_header.frequency,
    tracker.stat().n(),
    0);

  tracker.scan(request_header, response_header.stamp, _events_logger);
  RCLCPP_DEBUG(m_node_logging->get_logger(), "Request on %s request number %d received %lu us", service_name.c_str(), request_header.tracking_number, tracker.last());

  return response_header;
}

}
