#include "bot.h"

#include <Franka/help.h>
#include <Kin/F_qFeatures.h>
#include <Kin/viewer.h>
#include <KOMO/pathTools.h>
#include <Robotiq/RobotiqGripper.h>
#include <Control/timingOpt.h>
#include <Optim/MP_Solver.h>

//===========================================================================

BotOp::BotOp(rai::Configuration& C, bool useRealRobot){
  //-- launch arm(s) & gripper(s)
  bool useGripper = rai::getParameter<bool>("bot/useGripper", true);
  bool robotiq = rai::getParameter<bool>("bot/useRobotiq", true);
  rai::String useArm = rai::getParameter<rai::String>("bot/useArm", "both");

  C.ensure_indexedJoints();
  qHome = C.getJointState();
  state.set()->initZero(qHome.N);
  if(useRealRobot){
    if(useArm=="left"){
      robotL = make_unique<FrankaThread>(0, franka_getJointIndices(C,'l'), cmd, state);
      if(useGripper) gripperL = make_unique<FrankaGripper>(0);
    }else if(useArm=="right"){
      robotR = make_unique<FrankaThread>(1, franka_getJointIndices(C,'r'), cmd, state);
      if(useGripper) gripperR = make_unique<FrankaGripper>(1);
    }else if(useArm=="both"){
      robotL = make_unique<FrankaThread>(0, franka_getJointIndices(C,'l'), cmd, state);
      robotR = make_unique<FrankaThread>(1, franka_getJointIndices(C,'r'), cmd, state);
      if(useGripper){
        if(robotiq){
          gripperL = make_unique<RobotiqGripper>(0);
          gripperR = make_unique<RobotiqGripper>(1);
        }else{
          gripperL = make_unique<FrankaGripper>(0);
          gripperR = make_unique<FrankaGripper>(1);
        }
      }
    }else{
      HALT("you need a botUseArm configuration (right, left, both)");
    }
    {// if using franka gripper, do a homing?
      //FrankaGripper *fg = dynamic_cast<FrankaGripper*>(gripperL.get());
      //if(fg) fg->homing();
    }
  }else{
    robotL = make_unique<ControlEmulator>(C, cmd, state); //, StringA(), .001, 10.);
    if(useGripper) gripperL = make_unique<GripperEmulator>();
  }
  C.setJointState(get_q());

  //-- launch OptiTrack
  if(rai::getParameter<bool>("bot/useOptitrack", false)){
    optitrack = make_unique<rai::OptiTrack>();
    optitrack->pull(C);
  }

  C.watch(false, STRING("time: 0"));
}

BotOp::~BotOp(){
  if(robotL) robotL.release();
  if(robotR) robotR.release();
}

double BotOp::get_t(){
  return state.get()->time;
}

void BotOp::getState(arr& q, arr& qDot, double& time){
  auto stateGet = state.get();
  q = stateGet->q;
  qDot = stateGet->qDot;
  time = stateGet->time;
}

arr BotOp::get_q() {
  return state.get()->q;
}

arr BotOp::get_qDot() {
  return state.get()->qDot;
}

double BotOp::getTimeToEnd(){
  auto sp = std::dynamic_pointer_cast<rai::CubicSplineCtrlReference>(ref);
  if(!sp){
    LOG(-1) <<"can't get timeToEnd for non-spline mode";
    return 0.;
  }
  double ctrlTime = get_t();
  return sp->getEndTime() - ctrlTime;
}

bool BotOp::step(rai::Configuration& C, double waitTime){
  //update q state
  C.setJointState(state.get()->q);

  //update optitrack state
  if(optitrack) optitrack->pull(C);

//  C.gl()->raiseWindow();
  double ctrlTime = state.get()->time;
  keypressed = C.watch(false, STRING("time: "<<ctrlTime <<"\n[q or ESC to ABORT]"));
  if(keypressed) C.gl()->resetPressedKey();
  if(keypressed==13) return false;
  if(keypressed=='q' || keypressed==27) return false;
  auto sp = std::dynamic_pointer_cast<rai::CubicSplineCtrlReference>(ref);
  if(sp && ctrlTime>sp->getEndTime()) return false;
  if(waitTime>0.) rai::wait(waitTime);
  return true;
}

std::shared_ptr<rai::CubicSplineCtrlReference> BotOp::getSplineRef(){
  auto sp = std::dynamic_pointer_cast<rai::CubicSplineCtrlReference>(ref);
  if(!sp){
    setReference<rai::CubicSplineCtrlReference>();
    sp = std::dynamic_pointer_cast<rai::CubicSplineCtrlReference>(ref);
    CHECK(sp, "this is not a spline reference!")
  }
  return sp;
}

double BotOp::move(const arr& path, const arr& vels, const arr& times, bool override){
  CHECK_EQ(times.N, path.d0, "");
  CHECK_EQ(times.N, vels.d0, "");

  double ctrlTime = get_t();
  if(override){
    //LOG(1) <<"override: " <<ctrlTime <<" - " <<_times;
    getSplineRef()->overrideSmooth(path, vels, times, ctrlTime);
  }else{
    //LOG(1) <<"append: " <<ctrlTime <<" - " <<_times;
    getSplineRef()->append(path, vels, times, ctrlTime);
  }
  return ctrlTime+times.last();
}

double BotOp::move(const arr& path, const arr& times, bool override){
  arr _times=times;
  if(_times.N==1 && path.d0>1){
    _times = range(0., times.scalar(), path.d0-1);
    _times += _times(1);
  }
  if(_times.N){
    CHECK_EQ(_times.N, path.d0, "");
  }
  arr vels;
  if(path.d0==1){
    vels = zeros(1, path.d1);
  }else{ //use timing opt to decide on vels and, optionally, on timing
    arr q, qDot;
    getSplineRef()->eval(q, qDot, NoArr, getSplineRef()->getEndTime());
    q = path[0];
    qDot = zeros(q.N);

    arr tangents = getVelocities_centralDifference(path, .1);
    tangents.delRows(-1);
    bool optTau = (times.N==0);
    TimingProblem timingProblem(path, tangents, q, qDot, 1e1, {}, differencing(_times), optTau);
    MP_Solver solver;
    solver
        .setProblem(timingProblem.ptr())
        .setSolver(MPS_newton);
    solver.opt
        .set_stopTolerance(1e-4)
        .set_maxStep(1e0)
        .set_damping(1e-2);
    auto ret = solver.solve();
    //LOG(1) <<"timing f: " <<ret->f;
    timingProblem.getVels(vels);
    if(!_times.N) _times = integral(timingProblem.tau);
  }

  return move(path, vels, _times, override);
}

void BotOp::moveAutoTimed(const arr& path, double maxVel, double maxAcc){
  CHECK_GE(path.d0, 16, "this only works for smooth paths!");
  double D = getMinDuration(path, maxVel, maxAcc);
  arr times = range(0., D, path.d0-1);
  times += times(1);
  move(path, times);
}

double BotOp::moveLeap(const arr& q_target, double timeCost){
  arr q = get_q();
  arr qDot = get_qDot();
  double dist = length(q-q_target);
  double vel = scalarProduct(qDot, q_target-q)/dist;
  double T = (sqrt(6.*timeCost*dist+vel*vel) - vel)/timeCost;
  if(dist<1e-4 || T<.1) T=.1;
  return move(~q_target, {T}, true);
}

void BotOp::home(rai::Configuration& C){
    C.gl()->raiseWindow();
    arr q=get_q();
    if(maxDiff(q,qHome)>1e-3){
      moveLeap(qHome, 1.);
      while(step(C));
    }else{
      move(~qHome, {.1});
    }
}

void BotOp::hold(bool floating, bool damping){
  auto zref = std::dynamic_pointer_cast<ZeroReference>(ref);
  if(!zref){
    setReference<ZeroReference>();
    zref = std::dynamic_pointer_cast<ZeroReference>(ref);
    CHECK(zref, "this is not a spline reference!")
  }
  if(floating){
    zref->setPositionReference({});
    if(damping){
      zref->setVelocityReference({0.}); //{0.}: have a Kd with zero vel ref;
    }else{
      zref->setVelocityReference({}); //{}: have no Kd term at all; {1.} have a Kd term with velRef=velTrue (and friction compensation!)
    }
  }else{
    arr q = get_q();
    zref->setPositionReference(q);
    zref->setVelocityReference({0.});
  }
}

//===========================================================================

void ZeroReference::getReference(arr& q_ref, arr& qDot_ref, arr& qDDot_ref, const arr& q_real, const arr& qDot_real, double ctrlTime){
  {
    arr pos = position_ref.get()();
    if(pos.N) q_ref = pos;
    else q_ref.clear(); // = q_real;  //->no position gains at all
  }
  {
    arr vel = velocity_ref.get()();
    if(vel.N==1){
      double a = vel.scalar();
      CHECK(a>=0. && a<=1., "");
      qDot_ref = a * qDot_real; //[0] -> zero vel reference -> damping
    }
    else if(vel.N) qDot_ref = vel;
    else qDot_ref.clear(); //.clear();  //[] -> no damping at all! (and also no friction compensation based on reference qDot)
  }
  qDDot_ref.clear(); //[] -> no acc at all
}

