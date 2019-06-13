#include "ur_modern_driver/ros/action_server.h"
#include <cmath>

ActionServer::ActionServer(TrajectoryFollower& follower, std::vector<std::string>& joint_names, double max_velocity)
  : as_(nh_, "follow_joint_trajectory", boost::bind(&ActionServer::onGoal, this, _1),
        boost::bind(&ActionServer::onCancel, this, _1), false)
  , joint_names_(joint_names)
  , joint_set_(joint_names.begin(), joint_names.end())
  , max_velocity_(max_velocity)
  , interrupt_traj_(false)
  , has_goal_(false)
  , running_(false)
  , follower_(follower)
  , state_(RobotState::Error)
  , use_smooth_trajectory_(true)
  , kill_on_hang_(true)
{

}

void ActionServer::start()
{
  ros::NodeHandle pnh("~");
  pnh.param("use_smooth_trajectory", use_smooth_trajectory_, use_smooth_trajectory_);
  pnh.param("kill_on_hang", kill_on_hang_, kill_on_hang_);
  if (use_smooth_trajectory_)
  {
    LOG_INFO("Robot will execute smooth trajectories.");
  }
  else
  {
    LOG_WARN("Robot will stop at each trajectory point.");
  }

  if (running_)
    return;

  LOG_INFO("Starting ActionServer");
  running_ = true;
  tj_thread_ = thread(&ActionServer::trajectoryThread, this);
  as_.start();
}

void ActionServer::onRobotStateChange(RobotState state)
{
  state_ = state;

  // don't interrupt if everything is fine
  if (state == RobotState::Running)
    return;

  // don't retry interrupts
  if (interrupt_traj_ || !has_goal_)
    return;

  // on successful lock we're not executing a goal so don't interrupt
  if (tj_mutex_.try_lock())
  {
    tj_mutex_.unlock();
    return;
  }

  interrupt_traj_ = true;
  // wait for goal to be interrupted and automagically unlock when going out of scope
  std::lock_guard<std::mutex> lock(tj_mutex_);

  Result res;
  res.error_code = -100;
  res.error_string = "Robot safety stop";
  curr_gh_.setAborted(res, res.error_string);
}

bool ActionServer::updateState(RTShared& data)
{
  q_actual_ = data.q_actual;
  qd_actual_ = data.qd_actual;
  return true;
}

bool ActionServer::consume(RTState_V1_6__7& state)
{
  return updateState(state);
}
bool ActionServer::consume(RTState_V1_8& state)
{
  return updateState(state);
}
bool ActionServer::consume(RTState_V3_0__1& state)
{
  return updateState(state);
}
bool ActionServer::consume(RTState_V3_2__3& state)
{
  return updateState(state);
}
bool ActionServer::consume(RTState_V3_5__5_1& state)
{
  return updateState(state);
}

void ActionServer::onGoal(GoalHandle gh)
{
  Result res;
  res.error_code = -100;

  LOG_INFO("Received new goal");

  if (!validate(gh, res) || !try_execute(gh, res))
  {
    LOG_WARN("Goal error: %s", res.error_string.c_str());
    gh.setRejected(res, res.error_string);
  }
}

void ActionServer::onCancel(GoalHandle gh)
{
  interrupt_traj_ = true;
  // wait for goal to be interrupted
  std::lock_guard<std::mutex> lock(tj_mutex_);

  LOG_WARN("Trajectory has been canceled by client. Trajectory execution may "
           "have timed out. Check to make sure that the speed slider is set "
           "to 100%% on the pendant.");
  Result res;
  res.error_code = -100;
  res.error_string = "Goal cancelled by client";
  gh.setCanceled(res);
}

bool ActionServer::validate(GoalHandle& gh, Result& res)
{
  return validateState(gh, res) && validateJoints(gh, res) && validateTrajectory(gh, res);
}

bool ActionServer::validateState(GoalHandle& gh, Result& res)
{
  switch (state_)
  {
    case RobotState::EmergencyStopped:
      res.error_string = "Robot is emergency stopped";
      return false;

    case RobotState::ProtectiveStopped:
      res.error_string = "Robot is protective stopped";
      return false;

    case RobotState::Error:
      res.error_string = "Robot is not ready, check robot_mode";
      return false;

    case RobotState::Running:
      return true;

    default:
      res.error_string = "Undefined state";
      return false;
  }
}

bool ActionServer::validateJoints(GoalHandle& gh, Result& res)
{
  auto goal = gh.getGoal();
  auto const& joints = goal->trajectory.joint_names;
  std::set<std::string> goal_joints(joints.begin(), joints.end());

  if (goal_joints == joint_set_)
    return true;

  res.error_code = Result::INVALID_JOINTS;
  res.error_string = "Invalid joint names for goal\n";
  res.error_string += "Expected: ";
  std::for_each(goal_joints.begin(), goal_joints.end(), [&res](std::string joint){res.error_string += joint + ", ";});
  res.error_string += "\nFound: ";
  std::for_each(joint_set_.begin(), joint_set_.end(), [&res](std::string joint){res.error_string += joint + ", ";});
  return false;
}

bool ActionServer::validateTrajectory(GoalHandle& gh, Result& res)
{
  auto goal = gh.getGoal();
  res.error_code = Result::INVALID_GOAL;

  // must at least have one point
  if (goal->trajectory.points.size() < 1)
    return false;

  for (auto const& point : goal->trajectory.points)
  {
    if (point.velocities.size() != joint_names_.size())
    {
      res.error_code = Result::INVALID_GOAL;
      res.error_string = "Received a goal with an invalid number of velocities";
      return false;
    }

    if (point.positions.size() != joint_names_.size())
    {
      res.error_code = Result::INVALID_GOAL;
      res.error_string = "Received a goal with an invalid number of positions";
      return false;
    }

    for (auto const& velocity : point.velocities)
    {
      if (!std::isfinite(velocity))
      {
        res.error_string = "Received a goal with infinities or NaNs in velocity";
        return false;
      }
      if (std::fabs(velocity) > max_velocity_)
      {
        res.error_string = "Received a goal with velocities that are higher than max_velocity_ " + std::to_string(max_velocity_);
        return false;
      }
    }
    for (auto const& position : point.positions)
    {
      if (!std::isfinite(position))
      {
        res.error_string = "Received a goal with infinities or NaNs in positions";
        return false;
      }
    }
  }

  // todo validate start position?

  return true;
}

inline std::chrono::microseconds convert(const ros::Duration& dur)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(dur.sec) +
                                                               std::chrono::nanoseconds(dur.nsec));
}

bool ActionServer::try_execute(GoalHandle& gh, Result& res)
{
  if (!running_)
  {
    res.error_string = "Internal error";
    return false;
  }
  if (!tj_mutex_.try_lock())
  {
    interrupt_traj_ = true;
    res.error_string = "Received another trajectory";
    curr_gh_.setAborted(res, res.error_string);
    tj_mutex_.lock();
    // todo: make configurable
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  // locked here
  curr_gh_ = gh;
  interrupt_traj_ = false;
  has_goal_ = true;
  tj_mutex_.unlock();
  tj_cv_.notify_one();
  return true;
}

std::vector<size_t> ActionServer::reorderMap(std::vector<std::string> goal_joints)
{
  std::vector<size_t> indecies;
  for (auto const& aj : joint_names_)
  {
    size_t j = 0;
    for (auto const& gj : goal_joints)
    {
      if (aj == gj)
        break;
      j++;
    }
    indecies.push_back(j);
  }
  return indecies;
}

void ActionServer::trajectoryThread()
{
  LOG_INFO("Trajectory thread started");

  while (running_)
  {
    std::unique_lock<std::mutex> lk(tj_mutex_);
    if (!tj_cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return running_ && has_goal_; }))
      continue;
  
    LOG_INFO("Trajectory received and accepted");
    curr_gh_.setAccepted();

    auto goal = curr_gh_.getGoal();
    std::vector<TrajectoryPoint> trajectory;
    trajectory.reserve(goal->trajectory.points.size() + 1);

    // joint names of the goal might have a different ordering compared
    // to what URScript expects so need to map between the two
    auto mapping = reorderMap(goal->trajectory.joint_names);

    LOG_INFO("Translating trajectory");

    auto const& fp = goal->trajectory.points[0];
    auto fpt = convert(fp.time_from_start);

    // make sure we have a proper t0 position
    if (fpt > std::chrono::microseconds(0))
    {
      LOG_INFO("Trajectory without t0 recieved, inserting t0 at currrent position");
      trajectory.push_back(TrajectoryPoint(q_actual_, qd_actual_, std::chrono::microseconds(0)));
    }

    for (auto const& point : goal->trajectory.points)
    {
      std::array<double, 6> pos, vel;
      for (size_t i = 0; i < 6; i++)
      {
        size_t idx = mapping[i];
        pos[idx] = point.positions[i];
        vel[idx] = point.velocities[i];
      }
      auto t = convert(point.time_from_start);
      trajectory.push_back(TrajectoryPoint(pos, vel, t));
    }

    double t =
      std::chrono::duration_cast<std::chrono::duration<double>>(
        trajectory[trajectory.size() - 1].time_from_start).count();
    LOG_INFO("Executing trajectory with %zu points and duration of %4.3fs",
             trajectory.size(), t);

    Result res;

    if (use_smooth_trajectory_)
    {
      if (!follower_.startSmoothTrajectory(trajectory))
      {
	LOG_WARN("Robot has hung.");
	res.error_code = -100;
	res.error_string = "Robot has hung. ";
	curr_gh_.setAborted(res, res.error_string);	

        if (kill_on_hang_)
        {
          LOG_ERROR("Preparing to kill the robot driver. Note that the driver "
                    "can recover if it is configured to automatically respawn.");

          ros::Duration(0.25).sleep();
          exit(0);
        }
      }
      
      // Wait until the trajectory completes or times out.
      // t is the total time, so use 1.5*t as the timeout. The action client
      // can enforce a shorter timeout if necessary.
      double timeout_duration = t * 1.5;
      ros::Time timeout = ros::Time::now() + ros::Duration(timeout_duration);
      bool timed_out = true;
      // Don't start checking immediately (in case the trajectory ends at the 
      // start location), but also don't wait too long so that the trajectory
      // can be pre-empted as soon as necessary.
      ros::Duration(t * 0.1).sleep();
      while (ros::Time::now() < timeout || inMotion())
      {
        if (reachedGoal(trajectory.back()) && !inMotion())
        {
          timed_out = false;
	  LOG_INFO("Trajectory executed successfully");
	  res.error_code = Result::SUCCESSFUL;
	  curr_gh_.setSucceeded(res);
          break;
        }

        if (interrupt_traj_)
        {
          LOG_WARN("Trajectory interrupted");
          timed_out = false;
          break;
        }
        usleep(1000);
      }

      if ( timed_out )
      {
	LOG_ERROR("Trajectory timed out or failed to reach goal!");
	res.error_code = -100;
	res.error_string = "Robot motion timed out or failed to reach goal.";
	curr_gh_.setAborted(res, res.error_string);	
      }
      follower_.stop();
      has_goal_ = false;
      lk.unlock();
    }
    else
    {
      if (follower_.startTimedTrajectory(trajectory))
      {
        if (!interrupt_traj_)
        {
	  LOG_INFO("Trajectory executed successfully");
	  res.error_code = Result::SUCCESSFUL;
	  curr_gh_.setSucceeded(res);
	}
	else
	{
	    LOG_INFO("Trajectory interrupted");
	}
	follower_.stop();
      }
      else
      {
        LOG_WARN("Robot has hung.");
        res.error_code = -100;
        res.error_string = "Robot has hung. ";
        curr_gh_.setAborted(res, res.error_string);	
        
        if (kill_on_hang_)
        {
          LOG_ERROR("Preparing to kill the robot driver. Note that the driver "
                    "can recover if it is configured to automatically respawn.");

          ros::Duration(0.25).sleep();
          exit(0);
        }
      }
      
      has_goal_ = false;
      lk.unlock();
    }
  }
}

bool ActionServer::reachedGoal(const TrajectoryPoint& goal_point)
{
  // Pick a smaller tolerance than we're likely to use at a higher level
  const double tolerance = 0.0025;
  // Check to see if the joint angles are close to the goal
  for (size_t i = 0; i < q_actual_.size(); ++i)
  {
    if (std::abs(q_actual_[i] - goal_point.positions[i]) > tolerance)
    {
      return false;
    }
  }
  return true;
}

  
bool ActionServer::inMotion()
{
  // Check the joint velocities to see whether the robot is still in motion
  for (auto joint_speed : qd_actual_)
  {
    // TODO: The threshold value here is just a guess. It should be replaced
    //       with a more realistic value based on the specs/performance of
    //       the robot.
    if (std::abs(joint_speed) > 0.01)
    {
      return true;
    }
  }
  return false;
}

