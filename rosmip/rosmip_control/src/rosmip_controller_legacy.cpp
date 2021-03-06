#include <cmath>
#include <boost/assign.hpp>
#include <tf/transform_datatypes.h>

#include <rosmip_control/rosmip_legacy_controller.h>

//#define DISABLE_MOTORS
namespace rosmip_controller {

 
  double saturate(double _v, double _min, double _max) {
    if (_v < _min) return _min;
    if (_v > _max) return _max;
    return _v;
  }

  
#define __NAME "rosmip_balance_controller"
#define THETA_0	          -0.2
#define ENCODER_CHANNEL_L  1
#define ENCODER_CHANNEL_R  2
#define WHEEL_RADIUS_M     0.03
//#define WHEEL_TRACK_M      (0.083+0.01)
#define WHEEL_TRACK_M      (0.083)

#define SOFT_START_SEC     0.7
#define DT                 0.01
// inner loop controller 100hz
#define D1_GAIN	           1.05
#define D1_ORDER           2
#define D1_NUM             {-4.945, 8.862, -3.967}
#define D1_DEN             { 1.000, -1.481, 0.4812}
// outer loop controller new 100hz
#define D2_GAIN            0.9
#define	D2_ORDER           2
#define D2_NUM             {0.18856,  -0.37209,  0.18354}
#define D2_DEN	           {1.00000,  -1.86046,   0.86046}
#define THETA_REF_MAX      0.33
// steering controller
#define D3_KP              1.0
#define D3_KI              0.3
#define D3_KD              0.05
#define STEERING_INPUT_MAX 0.5

  
/*******************************************************************************
 *
 *
 *******************************************************************************/
RosMipLegacyController::RosMipLegacyController():
  enable_odom_tf_(true),
  state_est_(WHEEL_RADIUS_M, WHEEL_TRACK_M)
  {
    ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::RosMipLegacyController...");

    // set up D1 Theta controller
    D1_ = rc_empty_filter();
    float D1_num[] = D1_NUM;
    float D1_den[] = D1_DEN;
    rc_alloc_filter_from_arrays(&D1_, D1_ORDER, DT, D1_num, D1_den);
    D1_.gain = D1_GAIN;
    rc_enable_saturation(&D1_, -1.0, 1.0);
    rc_enable_soft_start(&D1_, SOFT_START_SEC);

    // set up D2 Phi controller
    D2_ =rc_empty_filter();
    float D2_num[] = D2_NUM;
    float D2_den[] = D2_DEN;
    rc_alloc_filter_from_arrays(&D2_, D2_ORDER, DT, D2_num, D2_den);
    D2_.gain = D2_GAIN;
    rc_enable_saturation(&D2_, -THETA_REF_MAX, THETA_REF_MAX);
    rc_enable_soft_start(&D2_, SOFT_START_SEC);

    // set up D3 gamma (steering) controller
    D3_ =rc_empty_filter();
    rc_pid_filter(&D3_, D3_KP, D3_KI, D3_KD, 4*DT, DT);
    rc_enable_saturation(&D3_, -STEERING_INPUT_MAX, STEERING_INPUT_MAX);
    
}

/*******************************************************************************
 *
 *
 *******************************************************************************/
RosMipLegacyController:: ~RosMipLegacyController() {
    ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::~RosMipLegacyController...");
}


#define SQR(_a) (_a*_a)
/*******************************************************************************
 *
 *
 *******************************************************************************/
bool RosMipLegacyController::init(hardware_interface::RobotHW* hw,
				    ros::NodeHandle& root_nh,
				    ros::NodeHandle& controller_nh)
{

    ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::init...");
    hw_ = static_cast<RosMipHardwareInterface*>(hw);
    hardware_interface::EffortJointInterface* e = hw->get<hardware_interface::EffortJointInterface>();
    left_wheel_joint_  = e->getHandle("left_wheel_joint");
    right_wheel_joint_ = e->getHandle("right_wheel_joint");

    hardware_interface::ImuSensorInterface* i = hw->get<hardware_interface::ImuSensorInterface>();
    imu_ = i->getHandle("imu");

    //hardware_interface::DsmInterface* d = hw->get<hardware_interface::DsmInterface>();
    //dsm_ = d->getHandle("dsm");
    
    state_est_.init();
    inp_mng_.init(hw, controller_nh);

    controller_nh.param("enable_odom_tf", enable_odom_tf_, enable_odom_tf_);
    ROS_INFO_STREAM_NAMED(__NAME, "Publishing to tf is " << (enable_odom_tf_?"enabled":"disabled"));
    
    debug_pub_.reset(new realtime_tools::RealtimePublisher<rosmip_control::debug>(controller_nh, "debug", 100));
    const std::string base_frame_id_ = "base_link";
    const std::string odom_frame_id_ = "odom";
    tf_odom_pub_.reset(new realtime_tools::RealtimePublisher<tf::tfMessage>(root_nh, "/tf", 100));
    tf_odom_pub_->msg_.transforms.resize(1);
    tf_odom_pub_->msg_.transforms[0].transform.translation.z = 0.0;
    tf_odom_pub_->msg_.transforms[0].child_frame_id = base_frame_id_;
    tf_odom_pub_->msg_.transforms[0].header.frame_id = odom_frame_id_;

    //
    double pose_cov_list[6] = {SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1)};
    double twist_cov_list[6] = {SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1), SQR(0.1)};
    
    odom_pub_.reset(new realtime_tools::RealtimePublisher<nav_msgs::Odometry>(controller_nh, "odom", 100));
    odom_pub_->msg_.header.frame_id = odom_frame_id_;
    odom_pub_->msg_.child_frame_id = base_frame_id_;
    odom_pub_->msg_.pose.pose.position.z = 0;
    odom_pub_->msg_.pose.covariance = boost::assign::list_of
      (static_cast<double>(pose_cov_list[0])) (0)  (0)  (0)  (0)  (0)
      (0)  (static_cast<double>(pose_cov_list[1])) (0)  (0)  (0)  (0)
      (0)  (0)  (static_cast<double>(pose_cov_list[2])) (0)  (0)  (0)
      (0)  (0)  (0)  (static_cast<double>(pose_cov_list[3])) (0)  (0)
      (0)  (0)  (0)  (0)  (static_cast<double>(pose_cov_list[4])) (0)
      (0)  (0)  (0)  (0)  (0)  (static_cast<double>(pose_cov_list[5]));
    odom_pub_->msg_.twist.twist.linear.y  = 0;
    odom_pub_->msg_.twist.twist.linear.z  = 0;
    odom_pub_->msg_.twist.twist.angular.x = 0;
    odom_pub_->msg_.twist.twist.angular.y = 0;
    odom_pub_->msg_.twist.covariance = boost::assign::list_of
      (static_cast<double>(twist_cov_list[0])) (0)  (0)  (0)  (0)  (0)
      (0)  (static_cast<double>(twist_cov_list[1])) (0)  (0)  (0)  (0)
      (0)  (0)  (static_cast<double>(twist_cov_list[2])) (0)  (0)  (0)
      (0)  (0)  (0)  (static_cast<double>(twist_cov_list[3])) (0)  (0)
      (0)  (0)  (0)  (0)  (static_cast<double>(twist_cov_list[4])) (0)
      (0)  (0)  (0)  (0)  (0)  (static_cast<double>(twist_cov_list[5]));
    
 
    ROS_INFO_STREAM_NAMED(__NAME, "leaving RosMipLegacyController::init...");
    return true;
}

/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::starting(const ros::Time& now) {
  ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::starting...");
  state_est_.starting(now, imu_.getOrientation(), left_wheel_joint_.getPosition(), right_wheel_joint_.getPosition());
}
  
/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::update(const ros::Time& now, const ros::Duration& dt) {
  // state estimation
  state_est_.update(now, imu_.getOrientation(), left_wheel_joint_.getPosition(), right_wheel_joint_.getPosition());

  core_state_.theta = state_est_.inertial_pitch_ + THETA_0;
  core_state_.wheelAngleL = left_wheel_joint_.getPosition();
  core_state_.wheelAngleR = right_wheel_joint_.getPosition();
  core_state_.phi = ((core_state_.wheelAngleL+core_state_.wheelAngleR)/2) + core_state_.theta;
  core_state_.gamma = (core_state_.wheelAngleR-core_state_.wheelAngleL) * (WHEEL_RADIUS_M/WHEEL_TRACK_M);
  tip_mon_.update(core_state_.theta);
 
  if (tip_mon_.prev_status_ == TIPPED and tip_mon_.status_ == UPRIGHT) {
    resetControlLaw();
    hw_->switch_motors_on();
    ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::update... switching motors on");
  }
  if (tip_mon_.prev_status_ == UPRIGHT and tip_mon_.status_ == TIPPED) {
    hw_->switch_motors_off();
    ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::update... switching motors off");
  }

  inp_mng_.update(now);
  setpoint_.phi_dot = inp_mng_.rt_commands_.lin/WHEEL_RADIUS_M;
  setpoint_.gamma_dot = inp_mng_.rt_commands_.ang;
  
  
  setpoint_.phi += setpoint_.phi_dot*DT;
  core_state_.d2_u = rc_march_filter(&D2_, setpoint_.phi-core_state_.phi);
  setpoint_.theta = core_state_.d2_u;
  
  //D1.gain = D1_GAIN * V_NOMINAL/cstate.vBatt;
  core_state_.d1_u = rc_march_filter(&D1_, (setpoint_.theta-core_state_.theta));
  
  //ROS_INFO("  in RosMipLegacyController::update... %f", dt.toSec());
  setpoint_.gamma += setpoint_.gamma_dot * DT;
  core_state_.d3_u = rc_march_filter(&D3_, setpoint_.gamma - core_state_.gamma);
  
  core_state_.dutyL = core_state_.d1_u - core_state_.d3_u;
  core_state_.dutyR = core_state_.d1_u + core_state_.d3_u;
  
#ifdef DISABLE_MOTORS
  left_wheel_joint_.setCommand(0);
  right_wheel_joint_.setCommand(0);
#else
  left_wheel_joint_.setCommand(core_state_.dutyL);
  right_wheel_joint_.setCommand(core_state_.dutyR);
#endif
  publishOdometry(now);
  publishDebug(now);
  
}

/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::publishOdometry(const ros::Time& now) {
  // Publish tf /odom frame
  //  const geometry_msgs::Quaternion orientation(
  //	tf::createQuaternionMsgFromYaw(state_est_.yaw_));
  const geometry_msgs::Quaternion
    orientation(tf::createQuaternionMsgFromRollPitchYaw(state_est_.inertial_roll_, state_est_.inertial_pitch_, state_est_.odom_yaw_));
  if (enable_odom_tf_ && tf_odom_pub_->trylock())
    {
      geometry_msgs::TransformStamped& odom_frame = tf_odom_pub_->msg_.transforms[0];
      odom_frame.header.stamp = now;
      odom_frame.transform.translation.x = state_est_.x_;
      odom_frame.transform.translation.y = state_est_.y_;
      //      odom_frame.transform.rotation.x = state_est_.q_odom_to_base_.x();
      //      odom_frame.transform.rotation.y = state_est_.q_odom_to_base_.y();
      //      odom_frame.transform.rotation.z = state_est_.q_odom_to_base_.z();
      //      odom_frame.transform.rotation.w = state_est_.q_odom_to_base_.w();
      odom_frame.transform.rotation = orientation;
      tf_odom_pub_->unlockAndPublish();
    }

 
  if (odom_pub_->trylock())
    {
      odom_pub_->msg_.header.stamp = now;
      odom_pub_->msg_.pose.pose.position.x = state_est_.x_;
      odom_pub_->msg_.pose.pose.position.y = state_est_.y_;
      odom_pub_->msg_.pose.pose.orientation = orientation;
      odom_pub_->msg_.twist.twist.linear.x  = state_est_.linear_;
      odom_pub_->msg_.twist.twist.angular.z = state_est_.angular_;
      odom_pub_->unlockAndPublish();
    }


  
}

/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::publishDebug(const ros::Time& now) {
  if (debug_pub_->trylock()) {
    //debug_pub_->msg_.header.stamp = now;
    debug_pub_->msg_.gamma = core_state_.gamma;
    debug_pub_->msg_.theta = core_state_.theta;
    //debug_pub_->msg_.thetad = thetad;
    debug_pub_->msg_.phiL = core_state_.wheelAngleL;
    debug_pub_->msg_.phiR = core_state_.wheelAngleR;
    //debug_pub_->msg_.phidL = phidL;
    //debug_pub_->msg_.phidR = phidR;
    debug_pub_->msg_.gamma_sp = setpoint_.gamma;
    debug_pub_->msg_.theta_sp = setpoint_.theta;
    debug_pub_->msg_.phiL_sp = setpoint_.phi;
    debug_pub_->msg_.phiR_sp = setpoint_.gamma;
    debug_pub_->msg_.tauL = core_state_.dutyL;
    debug_pub_->msg_.tauR = core_state_.dutyR;
    debug_pub_->unlockAndPublish();
  }
}


/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::resetControlLaw() {
  rc_reset_filter(&D1_);
  rc_reset_filter(&D2_);
  rc_reset_filter(&D3_);
  setpoint_.theta = 0.0f;
  setpoint_.phi   = 0.0f;
  setpoint_.gamma = 0.0f;
  rc_set_motor_all(0.0f);
  rc_set_encoder_pos(ENCODER_CHANNEL_L,0);
  rc_set_encoder_pos(ENCODER_CHANNEL_R,0);
}

/*******************************************************************************
 *
 *
 *******************************************************************************/
void RosMipLegacyController::stopping(const ros::Time&) {
  ROS_INFO_STREAM_NAMED(__NAME, "in RosMipLegacyController::stop...");
  left_wheel_joint_.setCommand(0);
  right_wheel_joint_.setCommand(0);
}
  
  
  PLUGINLIB_EXPORT_CLASS(rosmip_controller::RosMipLegacyController, controller_interface::ControllerBase);
}
