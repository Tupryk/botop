#pragma once

#include <Kin/kin.h>
#include <KOMO/komo.h>
#include <Control/CtrlMsgs.h>
#include <Franka/franka.h>
#include <Franka/gripper.h>
#include <Franka/controlEmulator.h>

//===========================================================================

struct ZeroReference : rai::ReferenceFeed {
  Var<arr> position_ref; ///< if set, defines a non-zero velocity reference
  Var<arr> velocity_ref; ///< if set, defines a non-zero velocity reference

  ZeroReference& setVelocityReference(const arr& _velocity_ref){ velocity_ref.set() = _velocity_ref; return *this; }
  ZeroReference& setPositionReference(const arr& _position_ref){ position_ref.set() = _position_ref; return *this; }

  /// callback called by a robot control loop
  virtual void getReference(arr& q_ref, arr& qDot_ref, arr& qDDot_ref, const arr& q_real, const arr& qDot_real, double ctrlTime);
};

//===========================================================================

struct BotOp{
  std::unique_ptr<rai::RobotAbstraction> robot;
  std::unique_ptr<rai::GripperAbstraction> gripper;
  std::shared_ptr<rai::ReferenceFeed> ref;
  arr qHome;
  int keypressed=0;

  BotOp(rai::Configuration& C, bool useRealRobot);
  ~BotOp();

  template<class T> BotOp& setReference();

  double get_t();
  const arr& get_qHome(){ return qHome; }
  arr get_q();
  arr get_qDot();
  double getTimeToEnd();

  bool step(rai::Configuration& C, double waitTime=.1);

  void moveLeap(const arr& q_target, double timeCost=.7);
  void move(const arr& path, double duration);

  void hold(bool floating=true, bool damping=true);
};

//===========================================================================

template<class T> BotOp& BotOp::setReference(){
  //comment the next line to only get gravity compensation instead of 'zero reference following' (which includes damping)
  ref = make_shared<T>();
  robot->cmd.set()->ref = ref;
//  ref->setPositionReference(q_now);
//ref->setVelocityReference({.0,.0,.2,0,0,0,0});
  return *this;
}

//===========================================================================

arr getLoopPath(rai::Configuration& C);