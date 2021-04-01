#include "franka.h"

#include <franka/model.h>
#include <franka/robot.h>

void naturalGains(double& Kp, double& Kd, double decayTime, double dampingRatio);

const char *frankaIpAddresses[2] = {"172.16.0.2", "172.17.0.2"};

FrankaThread::FrankaThread(Var<CtrlMsg>& _ctrl, Var<CtrlMsg>& _state, uint whichRobot, const uintA& _qIndices)
  : Thread("FrankaThread"),
    ctrl(_ctrl),
    ctrl_state(_state),
    qIndices(_qIndices){
  if(qIndices.N){
    CHECK_EQ(qIndices.N, 7, "");
    qIndices_max = qIndices.max();
  }

  //-- basic Kp Kd settings
  Kp_freq = rai::getParameter<arr>("Franka/Kp_freq", ARR(15., 15., 15., 10., 5., 5., 3.));
  Kd_ratio = rai::getParameter<arr>("Franka/Kd_ratio", ARR(.8, .8, .7, .7, .1, .1, .1));
  LOG(0) <<"FRANKA: Kp_freq=" <<Kp_freq <<" Kd_ratio=" <<Kd_ratio;

  //-- choose robot/ipAddress
  CHECK_LE(whichRobot, 1, "");
  ipAddress = frankaIpAddresses[whichRobot];

  //-- start thread and wait for first state signal
  threadStep();  //this is not looping! The step method passes a callback to robot.control, which is blocking until stop becomes true
  while(firstTime) rai::wait(.01);
}

FrankaThread::~FrankaThread(){
  stop = true;
  rai::wait(.1);
  threadClose();
}

void FrankaThread::step(){
  // connect to robot
  franka::Robot robot(ipAddress);

  // load the kinematics and dynamics model
  franka::Model model = robot.loadModel();

  // set collision behavior
  robot.setCollisionBehavior({{100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
  {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
  {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
  {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0}});

  // initialize state and ctrl with first state
  {
    franka::RobotState initial_state = robot.readOnce();
    arr q, qdot;
    q.setCarray(initial_state.q.begin(), initial_state.q.size());
    qdot.setCarray(initial_state.dq.begin(), initial_state.dq.size());

    auto stateset = ctrl_state.set();
    auto ref = ctrl.set();
    if(!qIndices.N){
      stateset->q = q;
      stateset->qdot = qdot;
      ref->q = q;
      ref->qdot = zeros(q.N);
    }else{
      while(stateset->q.N<=qIndices_max) stateset->q.append(0.);
      while(stateset->qdot.N<=qIndices_max) stateset->qdot.append(0.);
      while(stateset->u_bias.N<=qIndices_max) stateset->u_bias.append(0.);
      while(ref->q.N<=qIndices_max) ref->q.append(0.);
      while(ref->qdot.N<=qIndices_max) ref->qdot.append(0.);
      for(uint i=0;i<7;i++){
        stateset->q(qIndices(i)) = q(i);
        stateset->qdot(qIndices(i)) = qdot(i);
        stateset->u_bias(qIndices(i)) = 0.;
        ref->q(qIndices(i)) = q(i);
        ref->qdot(qIndices(i)) = 0.;
      }
    }
  }



  // define callback for the torque control loop
  std::function<franka::Torques(const franka::RobotState&, franka::Duration)>
      torque_control_callback = [&](const franka::RobotState& robot_state,
                                   franka::Duration /*duration*/) -> franka::Torques {

    steps++;

    //-- get current state
    arr q, qdot, torques;
    q.setCarray(robot_state.q.begin(), robot_state.q.size());
    qdot.setCarray(robot_state.dq.begin(), robot_state.dq.size());
    torques.setCarray(robot_state.tau_ext_hat_filtered.begin(), robot_state.tau_ext_hat_filtered.size());
    //publish state
    {
      auto stateset = ctrl_state.set();
      if(!qIndices.N){
        stateset->q = q;
        stateset->qdot = qdot;
        stateset->u_bias = torques;
      }else{
        for(uint i=0;i<7;i++){
          stateset->q(qIndices(i)) = q(i);
          stateset->qdot(qIndices(i)) = qdot(i);
          stateset->u_bias(qIndices(i)) = torques(i);
        }
      }
    }

    //-- get current ctrl
    arr q_ref, qdot_ref, qdd_des, P_compliance;
    {
      auto ref = ctrl.get();
      if(!qIndices.N){
        q_ref = ref->q;
        qdot_ref = ref->qdot;
      }else{
        if(q_ref.N!=7) q_ref.resize(7);
        if(qdot_ref.N!=7) qdot_ref.resize(7);
        for(uint i=0;i<7;i++){
          q_ref(i) = ref->q(qIndices(i));
          qdot_ref(i) = ref->qdot(qIndices(i));
        }
      }
      P_compliance = ref->P_compliance;
      qdd_des = zeros(7);
    }

    firstTime=false;

    //check for correct ctrl otherwise do something...
    if(q_ref.N!=7){
      cerr <<"FRANKA: inconsistent ctrl q_ref message" <<endl;
      return std::array<double, 7>({0., 0., 0., 0., 0., 0., 0.});
    }
    if(P_compliance.N){
      if(!(P_compliance.nd==2 && P_compliance.d0==7 && P_compliance.d1==7)){
        cerr <<"FRANKA: inconsistent ctrl P_compliance message" <<endl;
        P_compliance.clear();
      }
    }

    //-- compute desired torques
    arr Kp(7), Kd(7);
    CHECK_EQ(Kp.N, 7,"");
    CHECK_EQ(Kd.N, 7,"");
    CHECK_EQ(Kp_freq.N, 7,"");
    CHECK_EQ(Kd_ratio.N, 7,"");
    for(uint i=0;i<7;i++){
      double freq = Kp_freq(i);
      Kp(i) = freq*freq;
      Kd(i) = 2.*Kd_ratio(i)*freq;
    }

    if(P_compliance.N){
      Kp = P_compliance * (Kp % P_compliance);
    }else{
      Kp = diag(Kp);
    }

    if(q_ref.N==7){
      qdd_des += Kp * (q_ref - q) + Kd % (qdot_ref - qdot);
//      if(!(steps%50)) cout <<"dot_ref" <<qdot_ref <<endl;
    }

#if 0
    arr M;
    M.setCarray(model.mass(robot_state).begin(), 49);
    M.reshape(7,7);

    arr u = M * qdd_des;
#else
    arr u = qdd_des;
#endif

    if(P_compliance.N) u = P_compliance * u;

    std::array<double, 7> u_array = {0., 0., 0., 0., 0., 0., 0.};
    std::copy(u.begin(), u.end(), u_array.begin());

    if(stop) return franka::MotionFinished(franka::Torques(u_array));
    return u_array;
  };

  // start real-time control loop
  robot.control(torque_control_callback);
}







//----------------------------------------------------------------------------------------------













FrankaThreadNew::FrankaThreadNew(Var<rai::CtrlCmdMsg>& _ctrl, Var<rai::CtrlStateMsg>& _state, uint whichRobot, const uintA& _qIndices)
  : Thread("FrankaThread"),
    ctrl(_ctrl),
    ctrl_state(_state),
    qIndices(_qIndices) {

  CHECK_EQ(qIndices.N, 7, "");
  qIndices_max = qIndices.max();

  //-- basic Kp Kd settings for reference control mode
  Kp_freq = rai::getParameter<arr>("Franka/Kp_freq", ARR(18., 18., 18., 13., 8., 8., 6.));
  Kd_ratio = rai::getParameter<arr>("Franka/Kd_ratio", ARR(.8, .8, .7, .7, .1, .1, .1));
  LOG(0) << "FRANKA: Kp_freq=" << Kp_freq << " Kd_ratio=" << Kd_ratio;

  //-- choose robot/ipAddress
  CHECK_LE(whichRobot, 1, "");
  ipAddress = frankaIpAddresses[whichRobot];

  //-- start thread and wait for first state signal
  threadStep();  //this is not looping! The step method passes a callback to robot.control, which is blocking until stop becomes true
  while(firstTime) rai::wait(.01);
}

FrankaThreadNew::~FrankaThreadNew(){
  stop = true;
  rai::wait(.1);
  threadClose();
}

long c = 0;

void FrankaThreadNew::step(){
  // connect to robot
  franka::Robot robot(ipAddress);

  // load the kinematics and dynamics model
  franka::Model model = robot.loadModel();

  // set collision behavior
  robot.setCollisionBehavior({{100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
                             {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
                             {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0}},
                             {{100.0, 100.0, 100.0, 100.0, 100.0, 100.0}});

  // initialize state and ctrl with first state
  {
    franka::RobotState initial_state = robot.readOnce();
    arr q, qdot;
    q.setCarray(initial_state.q.begin(), initial_state.q.size());
    qdot.setCarray(initial_state.dq.begin(), initial_state.dq.size());

    auto stateset = ctrl_state.set();
    auto ref = ctrl.set();

    // TODO really modify the complete state?
    while(stateset->q.N<=qIndices_max) stateset->q.append(0.);
    while(stateset->qDot.N<=qIndices_max) stateset->qDot.append(0.);
    while(stateset->tauExternal.N<=qIndices_max) stateset->tauExternal.append(0.);
//    while(ref->qRef.N<=qIndices_max) ref->qRef.append(0.);
//    while(ref->qDotRef.N<=qIndices_max) ref->qDotRef.append(0.);

    for(uint i=0; i < 7; i++){
      stateset->q(qIndices(i)) = q(i);
      stateset->qDot(qIndices(i)) = qdot(i);
      stateset->tauExternal(qIndices(i)) = 0.;
//      ref->qRef(qIndices(i)) = q(i);
//      ref->qDotRef(qIndices(i)) = 0.;
    }
  }





  // define callback for the torque control loop
  std::function<franka::Torques(const franka::RobotState&, franka::Duration)>
      torque_control_callback = [&](const franka::RobotState& robot_state,
                                   franka::Duration /*duration*/) -> franka::Torques {

    steps++;

    //-- get current state
    arr q, qdot, torques;
    q.setCarray(robot_state.q.begin(), robot_state.q.size());
    qdot.setCarray(robot_state.dq.begin(), robot_state.dq.size());
    torques.setCarray(robot_state.tau_ext_hat_filtered.begin(), robot_state.tau_ext_hat_filtered.size());

    //publish state
    {
      auto stateset = ctrl_state.set();
      for(uint i=0;i<7;i++){
        stateset->q(qIndices(i)) = q(i);
        stateset->qDot(qIndices(i)) = qdot(i);
        stateset->tauExternal(qIndices(i)) = torques(i);
      }
    }

    //-- get current ctrl command
    arr q_ref, qdot_ref, qDDotRef, KpRef, KdRef, P_compliance; // TODO Kp, Kd and also read out the correct indices
    rai::ControlType controlType;
    {
      auto ref = ctrl.get();

      controlType = ref->controlType;

      //get the reference from the callback (e.g., sampling a spline reference)
      CHECK(ref->ref, "the reference function is not set!");
      arr ref_qRef, ref_qDotRef, ref_qDDotRef;
      ref->ref(ref_qRef, ref_qDotRef, ref_qDDotRef, rai::realTime());

      if(ref_qRef.N >= 7) q_ref.resize(7).setZero();
      if(ref_qDotRef.N >= 7) qdot_ref.resize(7).setZero();
      if(ref_qDDotRef.N >= 7) qDDotRef.resize(7).setZero();
      if(ref->Kp.d0 >= 7 && ref->Kp.d1 >=7 && ref->Kp.d0 == ref->Kp.d1) KpRef.resize(7, 7);
      if(ref->Kd.d0 >= 7 && ref->Kd.d1 >=7 && ref->Kd.d0 == ref->Kd.d1) KdRef.resize(7, 7);

      for(uint i = 0; i < 7; i++) {
        if(ref_qRef.N >= 7) q_ref(i) = ref_qRef(qIndices(i));
        if(ref_qDotRef.N >= 7) qdot_ref(i) = ref_qDotRef(qIndices(i));
        if(ref_qDDotRef.N >= 7) qDDotRef(i) = ref_qDDotRef(qIndices(i));

        for(uint j = 0; j < 7; j++) {
          if(ref->Kp.d0 >= 7 && ref->Kp.d1 >=7 && ref->Kp.d0 == ref->Kp.d1) KpRef(i, j) = ref->Kp(qIndices(i), qIndices(j));
          if(ref->Kd.d0 >= 7 && ref->Kd.d1 >=7 && ref->Kd.d0 == ref->Kd.d1) KdRef(i, j) = ref->Kd(qIndices(i), qIndices(j));
        }
      }

      if(ref->P_compliance.N) {
        HALT("NOT IMPLEMENTED YET (at least properly)")
        P_compliance = ref->P_compliance;
      }

    }

    firstTime=false;

    arr u = zeros(7); // torques send to the robot

    //-- construct torques from control message depending on the control type
    if(controlType == rai::ControlType::configRefs) { // plain qRef, qDotRef references
      arr qdd_des = zeros(7);
      //check for correct ctrl otherwise do something...
      if(q_ref.N!=7){
        cerr << "FRANKA: inconsistent ctrl q_ref message" << endl;
        return std::array<double, 7>({0., 0., 0., 0., 0., 0., 0.});
      }
      if(P_compliance.N){
        if(!(P_compliance.nd==2 && P_compliance.d0==7 && P_compliance.d1==7)){
          cerr << "FRANKA: inconsistent ctrl P_compliance message" << endl;
          P_compliance.clear();
        }
      }
      //-- compute desired torques
      arr Kp(7), Kd(7);
      CHECK_EQ(Kp.N, 7,"");
      CHECK_EQ(Kd.N, 7,"");
      CHECK_EQ(Kp_freq.N, 7,"");
      CHECK_EQ(Kd_ratio.N, 7,"");
      for(uint i=0;i<7;i++){
        double freq = Kp_freq(i);
        Kp(i) = freq*freq;
        Kd(i) = 2.*Kd_ratio(i)*freq;
      }

      if(P_compliance.N){
        Kp = P_compliance * (Kp % P_compliance);
      }else{
        Kp = diag(Kp);
      }

      if(q_ref.N==7){
        qdd_des += Kp * (q_ref - q) + Kd % (qdot_ref - qdot);
        // if(!(steps%50)) cout <<"dot_ref" <<qdot_ref <<endl;
      }

      u = qdd_des;
      if(P_compliance.N) u = P_compliance * u;

    } else if(controlType == rai::ControlType::projectedAcc) { // projected Kp, Kd and u_b term for projected operational space control
      CHECK_EQ(KpRef.nd, 2, "")
      CHECK_EQ(KpRef.d0, 7, "")
      CHECK_EQ(KpRef.d1, 7, "")
      CHECK_EQ(KdRef.nd, 2, "")
      CHECK_EQ(KdRef.d0, 7, "")
      CHECK_EQ(KdRef.d1, 7, "")
      CHECK_EQ(qDDotRef.N, 7, "")

      arr M;
      M.setCarray(model.mass(robot_state).begin(), 49);
      M.reshape(7,7);

#if 0
      c++;
      if(c%10 == 0) {
        cout << M << endl << endl;
      }
#endif


#if 0
      M(4,4) = 0.2;
      M(5,5) = 0.2;
      M(6,6) = 0.1;
#else
      arr MDiag = diag(ARR(0.4, 0.3, 0.3, 0.4, 0.4, 0.4, 0.2));
      M = M + MDiag;
#endif

      KpRef = M*KpRef;
      KdRef = M*KdRef;
      qDDotRef = M*qDDotRef;

      u = qDDotRef - KpRef*q - KdRef*qdot;

      //u(5) *= 2.0;

      //cout << u << endl;

      //u *= 0.0; // useful for testing new stuff without braking the robot
    }


    std::array<double, 7> u_array = {0., 0., 0., 0., 0., 0., 0.};
    std::copy(u.begin(), u.end(), u_array.begin());

    if(stop) return franka::MotionFinished(franka::Torques(u_array));
    return u_array;
  };

  // start real-time control loop
  robot.control(torque_control_callback);
}
