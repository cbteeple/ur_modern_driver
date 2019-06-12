#pragma once

#include <inttypes.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include "ur_modern_driver/log.h"
#include "ur_modern_driver/ur/commander.h"
#include "ur_modern_driver/ur/server.h"

struct TrajectoryPoint
{
  std::array<double, 6> positions;
  std::array<double, 6> velocities;
  std::chrono::microseconds time_from_start;

  TrajectoryPoint()
  {
  }

  TrajectoryPoint(std::array<double, 6> &pos, std::array<double, 6> &vel, std::chrono::microseconds tfs)
    : positions(pos), velocities(vel), time_from_start(tfs)
  {
  }
};

class TrajectoryFollower
{
private:
  std::atomic<bool> running_;
  std::array<double, 6> last_positions_;
  URCommander &commander_;
  URServer server_;
  std::string reverse_ip_;
  int reverse_port_;

  double servoj_time_, servoj_lookahead_time_, servoj_gain_;
  double max_acceleration_;
  std::string program_;

  std::mutex mutex_;
  std::thread server_thread_;
  bool timeout_canceled_;

  template <typename T>
  size_t append(uint8_t *buffer, T &val)
  {
    size_t s = sizeof(T);
    std::memcpy(buffer, &val, s);
    return s;
  }

  void serverThread();
  bool execute(std::array<double, 6> &positions, bool keep_alive);
  double interpolate(double t, double T, double p0_pos, double p1_pos, double p0_vel, double p1_vel);
  bool computeVelocityAndAccel(double dphi, double dt,
			       double max_vel, double max_accel,
			       double& vel, double& accel);

public:
  TrajectoryFollower(URCommander &commander, std::string &reverse_ip, int reverse_port, bool version_3);

  bool start();
  bool startSmoothTrajectory(const std::vector<TrajectoryPoint> &trajectory);
  bool startTimedTrajectory(const std::vector<TrajectoryPoint> &trajectory);
  bool execute(std::array<double, 6> &positions);
  bool execute(std::vector<TrajectoryPoint> &trajectory, std::atomic<bool> &interrupt);
  void stop();
  void interrupt();
};
