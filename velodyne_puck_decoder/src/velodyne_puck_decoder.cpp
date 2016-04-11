/*
 * This file is part of velodyne_puck driver.
 *
 * The driver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the driver.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <velodyne_puck_decoder/velodyne_puck_decoder.h>

using namespace std;

namespace velodyne_puck_decoder {
VelodynePuckDecoder::VelodynePuckDecoder(
    ros::NodeHandle& n, ros::NodeHandle& pn):
  nh(n),
  pnh(pn),
  is_first_sweep(true),
  last_azimuth(0.0),
  sweep_start_time(0.0),
  packet_start_time(0.0),
  sweep_data(new velodyne_puck_msgs::VelodynePuckSweep()){
  return;
}

bool VelodynePuckDecoder::loadParameters() {
  pnh.param<double>("min_range", min_range, 0.5);
  pnh.param<double>("max_range", max_range, 100.0);
  pnh.param<double>("frequency", max_range, 20.0);
  return true;
}

bool VelodynePuckDecoder::createRosIO() {
  packet_sub = nh.subscribe<velodyne_puck_msgs::VelodynePuckPacket>(
      "velodyne_packet", 100, &VelodynePuckDecoder::packetCallback, this);
  sweep_pub = nh.advertise<velodyne_puck_msgs::VelodynePuckSweep>(
      "velodyne_sweep", 10);
  point_cloud_pub = nh.advertise<sensor_msgs::PointCloud2>(
      "velodyne_point_cloud", 10);
  return true;
}

bool VelodynePuckDecoder::initialize() {
  if (!loadParameters()) {
    ROS_ERROR("Cannot load all required parameters...");
    return false;
  }

  if (!createRosIO()) {
    ROS_ERROR("Cannot create ROS I/O...");
    return false;
  }

  // Fill in the altitude for each scan.
  for (size_t scan_idx = 0; scan_idx < 16; ++scan_idx) {
    size_t remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+8;
    sweep_data->scans[remapped_scan_idx].altitude = scan_altitude[scan_idx];
  }

  return true;
}

bool VelodynePuckDecoder::checkPacketValidity(const RawPacket* packet) {
  for (size_t blk_idx = 0; blk_idx < BLOCKS_PER_PACKET; ++blk_idx) {
    if (packet->blocks[blk_idx].header != UPPER_BANK) {
      ROS_WARN("Skip invalid VLP-16 packet: block %lu header is %x",
          blk_idx, packet->blocks[blk_idx].header);
      return false;
    }
  }
  return true;
}

void VelodynePuckDecoder::clearSweepData() {
  for (size_t i = 0; i < 16; ++i) {
    sweep_data->scans[i].points.clear();
  }
  return;
}

void VelodynePuckDecoder::decodePacket(const RawPacket* packet) {

  // Compute the azimuth angle for each firing.
  for (size_t fir_idx = 0; fir_idx < FIRINGS_PER_PACKET; fir_idx+=2) {
    size_t blk_idx = fir_idx / 2;
    firings[fir_idx].firing_azimuth = rawAzimuthToDouble(
        packet->blocks[blk_idx].rotation);
  }

  // Interpolate the azimuth values
  for (size_t fir_idx = 1; fir_idx < FIRINGS_PER_PACKET; fir_idx+=2) {
    size_t lfir_idx = fir_idx - 1;
    size_t rfir_idx = fir_idx + 1;

    if (fir_idx == FIRINGS_PER_PACKET - 1) {
      lfir_idx = fir_idx - 3;
      rfir_idx = fir_idx - 1;
    }

    double azimuth_diff = firings[rfir_idx].firing_azimuth -
      firings[lfir_idx].firing_azimuth;
    azimuth_diff = azimuth_diff < 0 ? azimuth_diff + 2*M_PI : azimuth_diff;

    firings[fir_idx].firing_azimuth =
      firings[fir_idx-1].firing_azimuth + azimuth_diff/2.0;
    firings[fir_idx].firing_azimuth  =
      firings[fir_idx].firing_azimuth > 2*M_PI ?
      firings[fir_idx].firing_azimuth-2*M_PI : firings[fir_idx].firing_azimuth;
  }

  // Fill in the distance and intensity for each firing.
  for (size_t blk_idx = 0; blk_idx < BLOCKS_PER_PACKET; ++blk_idx) {
    const RawBlock& raw_block = packet->blocks[blk_idx];

    for (size_t blk_fir_idx = 0; blk_fir_idx < FIRINGS_PER_BLOCK; ++blk_fir_idx){
      size_t fir_idx = blk_idx*FIRINGS_PER_BLOCK + blk_fir_idx;

      double azimuth_diff = 0.0;
      if (fir_idx < FIRINGS_PER_PACKET - 1)
        azimuth_diff = firings[fir_idx+1].firing_azimuth -
          firings[fir_idx].firing_azimuth;
      else
        azimuth_diff = firings[fir_idx].firing_azimuth -
          firings[fir_idx-1].firing_azimuth;

      for (size_t scan_fir_idx = 0; scan_fir_idx < SCANS_PER_FIRING; ++scan_fir_idx){
        size_t byte_idx = RAW_SCAN_SIZE * (
            SCANS_PER_FIRING*blk_fir_idx + scan_fir_idx);

        // Azimuth
        firings[fir_idx].azimuth[scan_fir_idx] = firings[fir_idx].firing_azimuth +
          (scan_fir_idx*DSR_TOFFSET/FIRING_TOFFSET) * azimuth_diff;

        // Distance
        TwoBytes raw_distance;
        raw_distance.bytes[0] = raw_block.data[byte_idx];
        raw_distance.bytes[1] = raw_block.data[byte_idx+1];
        firings[fir_idx].distance[scan_fir_idx] = static_cast<double>(
            raw_distance.distance) * DISTANCE_RESOLUTION;

        // Intensity
        firings[fir_idx].intensity[scan_fir_idx] = static_cast<double>(
            raw_block.data[byte_idx+2]);
      }
    }
  }
  return;
}

//TODO: Fill in the header of published msgs.
void VelodynePuckDecoder::packetCallback(
    const velodyne_puck_msgs::VelodynePuckPacketConstPtr& msg) {

  // Convert the msg to the raw packet type.
  const RawPacket* raw_packet = (const RawPacket*) (&(msg->data[0]));

  // Check if the packet is valid
  if (!checkPacketValidity(raw_packet)) return;

  // Decode the packet
  decodePacket(raw_packet);

  // Convert the packets
  size_t new_sweep_start = 0;
  do {
    if (firings[new_sweep_start].firing_azimuth < last_azimuth) break;
    else {
      last_azimuth = firings[new_sweep_start].firing_azimuth;
      ++new_sweep_start;
    }
  } while (new_sweep_start < FIRINGS_PER_PACKET);

  // The first sweep may not be complete. We will
  // wait for the second sweep in order to find the
  // 0 azimuth angle.
  size_t start_fir_idx = 0;
  size_t end_fir_idx = new_sweep_start;
  if (is_first_sweep &&
      new_sweep_start == FIRINGS_PER_PACKET) {
    return;
  } else {
    if (is_first_sweep) {
      ROS_INFO("Start publishing sweep data...");
      is_first_sweep = false;
      start_fir_idx = new_sweep_start;
      end_fir_idx = FIRINGS_PER_PACKET;
    }
  }

  for (size_t fir_idx = start_fir_idx; fir_idx < end_fir_idx; ++fir_idx) {
    for (size_t scan_idx = 0; scan_idx < SCANS_PER_FIRING; ++scan_idx) {
      // Check if the point is valid.
      if (!isPointInRange(firings[fir_idx].distance[scan_idx])) continue;

      // Convert the point to xyz coordinate
      double x = firings[fir_idx].distance[scan_idx] *
        cos_scan_altitude[scan_idx] * sin(firings[fir_idx].azimuth[scan_idx]);
      double y = firings[fir_idx].distance[scan_idx] *
        cos_scan_altitude[scan_idx] * cos(firings[fir_idx].azimuth[scan_idx]);
      double z = firings[fir_idx].distance[scan_idx] *
        sin_scan_altitude[scan_idx];

      double x_coord = y;
      double y_coord = -x;
      double z_coord = z;

      // Compute the time of the point
      double time = packet_start_time +
        FIRING_TOFFSET*fir_idx + DSR_TOFFSET*scan_idx;

      // Remap the index of the scan
      int remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+8;
      sweep_data->scans[remapped_scan_idx].points.push_back(
          velodyne_puck_msgs::VelodynePuckPoint());
      velodyne_puck_msgs::VelodynePuckPoint& new_point =
        sweep_data->scans[remapped_scan_idx].points[
        sweep_data->scans[remapped_scan_idx].points.size()-1];

      // Pack the data into point msg
      new_point.time = time;
      new_point.x = x_coord;
      new_point.y = y_coord;
      new_point.z = z_coord;
      new_point.azimuth = firings[fir_idx].azimuth[scan_idx];
      new_point.distance = firings[fir_idx].distance[scan_idx];
      new_point.intensity = firings[fir_idx].intensity[scan_idx];
    }
  }

  packet_start_time += FIRING_TOFFSET * (end_fir_idx-start_fir_idx);

  // A new sweep begins
  if (end_fir_idx != FIRINGS_PER_PACKET) {
    // Publish the last revolution
    sweep_pub.publish(sweep_data);
    clearSweepData();

    // Prepare the next revolution
    packet_start_time = 0.0;
    last_azimuth = firings[FIRINGS_PER_PACKET-1].firing_azimuth;

    start_fir_idx = end_fir_idx;
    end_fir_idx = FIRINGS_PER_PACKET;

    for (size_t fir_idx = start_fir_idx; fir_idx < end_fir_idx; ++fir_idx) {
      for (size_t scan_idx = 0; scan_idx < SCANS_PER_FIRING; ++scan_idx) {
        // Check if the point is valid.
        if (!isPointInRange(firings[fir_idx].distance[scan_idx])) continue;

        // Convert the point to xyz coordinate
        double x = firings[fir_idx].distance[scan_idx] *
          cos_scan_altitude[scan_idx] * sin(firings[fir_idx].azimuth[scan_idx]);
        double y = firings[fir_idx].distance[scan_idx] *
          cos_scan_altitude[scan_idx] * cos(firings[fir_idx].azimuth[scan_idx]);
        double z = firings[fir_idx].distance[scan_idx] *
          sin_scan_altitude[scan_idx];

        double x_coord = y;
        double y_coord = -x;
        double z_coord = z;

        // Compute the time of the point
        double time = packet_start_time +
          FIRING_TOFFSET*fir_idx + DSR_TOFFSET*scan_idx;

        // Remap the index of the scan
        int remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+8;
        sweep_data->scans[remapped_scan_idx].points.push_back(
            velodyne_puck_msgs::VelodynePuckPoint());
        velodyne_puck_msgs::VelodynePuckPoint& new_point =
          sweep_data->scans[remapped_scan_idx].points[
          sweep_data->scans[remapped_scan_idx].points.size()-1];

        // Pack the data into point msg
        new_point.time = time;
        new_point.x = x_coord;
        new_point.y = y_coord;
        new_point.z = z_coord;
        new_point.azimuth = firings[fir_idx].azimuth[scan_idx];
        new_point.distance = firings[fir_idx].distance[scan_idx];
        new_point.intensity = firings[fir_idx].intensity[scan_idx];
      }
    }

    packet_start_time += FIRING_TOFFSET * (end_fir_idx-start_fir_idx);
  }

  return;
}

} // end namespace velodyne_puck_decoder

