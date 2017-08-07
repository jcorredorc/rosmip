
#include "rosmip_control/state_estimation.h"

namespace rosmip_controller {

  StateEstimator::StateEstimator():
    wheel_separation_(0.04),
    wheel_radius_(0.03) {
    

    
  }

  void  StateEstimator::init() {
    
    
  }

  
  void  StateEstimator::starting(const ros::Time& time) {
    x_ = 0.;
    y_ = 0.;
    heading_ = 0.;
    
    left_wheel_old_pos_ = 0.;
    right_wheel_old_pos_ = 0.;
    timestamp_ = time;
    
  }
  
  void  StateEstimator::update(double left_pos, double right_pos, const ros::Time &time) {
    /// Get current wheel joint positions:
    const double left_wheel_cur_pos  = left_pos  * wheel_radius_;
    const double right_wheel_cur_pos = right_pos * wheel_radius_;

    const double dt = (time - timestamp_).toSec();
    timestamp_ = time;
    /// Estimate velocity of wheels using old and current position:
    const double left_wheel_est_vel  = left_wheel_cur_pos  - left_wheel_old_pos_;
    const double right_wheel_est_vel = right_wheel_cur_pos - right_wheel_old_pos_;
    
    /// Update old position with current:
    left_wheel_old_pos_  = left_wheel_cur_pos;
    right_wheel_old_pos_ = right_wheel_cur_pos;
    
    /// Compute linear and angular diff:
    const double linear  = (right_wheel_est_vel + left_wheel_est_vel) * 0.5 ;
    const double angular = (right_wheel_est_vel - left_wheel_est_vel) / wheel_separation_;

    /// Integrate odometry:
    integrate(linear, angular);

 
    //ROS_INFO("%f", dt);
  }


  void StateEstimator::integrate(double linear, double angular) {
    if (fabs(angular) < 1e-6) {
       const double direction = heading_ + angular * 0.5;
       /// Runge-Kutta 2nd order integration:
       x_       += linear * cos(direction);
       y_       += linear * sin(direction);
       heading_ += angular;
    }
    else {
      /// Exact integration (should solve problems when angular is zero):
      const double heading_old = heading_;
      const double r = linear/angular;
      heading_ += angular;
      x_       +=  r * (sin(heading_) - sin(heading_old));
      y_       += -r * (cos(heading_) - cos(heading_old));
    }
  }
  

}