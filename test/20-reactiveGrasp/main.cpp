#include <BotOp/bot.h>
#include <BotOp/motionHelpers.h>
#include <KOMO/pathTools.h>
#include <Optim/MP_Solver.h>
#include <Control/flagHunter.h>
#include <Core/thread.h>

//===========================================================================

struct ObjectiveL : rai::Array<shared_ptr<Objective>>{
  ptr<struct Objective> add(const ptr<Feature>& f, ObjectiveType type) {
    append(make_shared<Objective>(f, type, "joey", arr{}, intA{}) );
    return last();
  }

  ptr<struct Objective> add(const FeatureSymbol& feat,  const rai::Configuration& C, const StringA& frames,
                  ObjectiveType type, const arr& scale=NoArr, const arr& target=NoArr, int order=-1) {
    return add(make_feature(feat, frames, C, scale, target, order), type);
  }

  double maxError(const rai::Configuration& C, int verbose=0) const {
    double maxError = 0.;
    for(const auto& o: *this) {
      if(o->type==OT_ineq || o->type==OT_eq) {
        arr y = o->feat->eval(o->feat->getFrames(C));
        double m=0.;
        for(double& yi : y){
          if(o->type==OT_ineq && yi>m) m=yi;
          if(o->type==OT_eq  && fabs(yi)>m) m=fabs(yi);
        }
        if(verbose>0){
          LOG(0) <<"err: " <<m <<' ' <<o->name <<' ' <<o->feat->shortTag(C);
        }
        if(m>maxError) maxError=m;
      }
    }
    return maxError;
  }
};

//===========================================================================


struct ChainMPC{
  KOMO komo;
  uint steps=0;
  //result
  arr path;
  arr tau;
  bool feasible=false;

  void buildKOMO(rai::Configuration& C, rai::Array<ObjectiveL> _phiflag, rai::Array<ObjectiveL> _phirun, const arr& qHome={});

  void oldSetup(rai::Configuration& C, double timingScale=1., const arr& qHome={});

  void reinit(const arr& x, const arr& v);
  void reinit(const rai::Configuration& C);

  void solve();
};

void ChainMPC::buildKOMO(rai::Configuration& C, rai::Array<ObjectiveL> phiflag, rai::Array<ObjectiveL> phirun, const arr& qHome){
  komo.clearObjectives();
  CHECK_EQ(phiflag.N, phirun.N, "");
  uint K=phiflag.N;

  uint subSamples=0; //#subframes

  komo.setModel(C, false);
  komo.setTiming(K, subSamples+1, 2., 1);
  komo.setupPathConfig();

  komo.add_qControlObjective({}, komo.k_order, 1e0);
  if(qHome.N) komo.addObjective({}, FS_qItself, {}, OT_sos, {.1}, qHome);

  for(uint k=0;k<K;k++){
    for(auto& o: phiflag(k))  komo.addObjective({1.+double(k)}, o->feat, {}, o->type);
    for(auto& o: phirun(k))  komo.addObjective({double(k), 1.+double(k)}, o->feat, {}, o->type);
  }

  komo.reset();
  komo.initWithConstant(qHome);
  path = komo.getPath_qOrg();
  this->tau = komo.getPath_tau();
}

void getGraspLinePlane(arr& xLine, arr& yzPlane, FeatureSymbol& xyScalarProduct, FeatureSymbol& xzScalarProduct,
                       const rai::ArgWord& dir){
  if(dir==rai::_xAxis){
      xLine = {{1,3},{1,0,0}};
      yzPlane = {{2,3},{0,1,0,0,0,1}};
      xyScalarProduct = FS_scalarProductXY;
      xzScalarProduct = FS_scalarProductXZ;
  } else if(dir==rai::_yAxis){
      xLine = {{1,3},{0,1,0}};
      yzPlane = {{2,3},{1,0,0,0,0,1}};
      xyScalarProduct = FS_scalarProductXX;
      xzScalarProduct = FS_scalarProductXZ;
  } else if(dir==rai::_zAxis){
      xLine = {{1,3},{0,0,1}};
      yzPlane = {{2,3},{1,0,0,0,1,0}};
      xyScalarProduct = FS_scalarProductXX;
      xzScalarProduct = FS_scalarProductXY;
  }
}

void ChainMPC::oldSetup(rai::Configuration& C, double timingScale, const arr& qHome){
  const char* gripperName="l_gripper";
  const char* palmName="l_palm";
  const char* boxName="target";
  const char* tableName="plate";
  const char* arm1Name="l_panda_coll7";
  const char* arm2Name="l_panda_coll6";

  arr q0 = C.getJointState();

  //------ setup KOMO problem
  uint K=2; //keyframes
  uint subSamples=0; //#subframes

  komo.setModel(C, true);
  komo.setTiming(K, subSamples+1, 2., 1);
  komo.setupPathConfig();

  //specific taus..
//  arr tau(komo.pathConfig.frames.d0);
//  CHECK_EQ(tau.N, komo.k_order+K*(T+1), "");
//  tau = .1;
//  for(uint s=0;s<komo.k_order;s++) tau(s)=.01;
//  for(uint k=0;k<K;k++) tau(komo.k_order-1+(k+1)*(T+1)) = .01;
//  cout <<tau <<endl;
//  komo.pathConfig.setTaus(tau);
//  komo.pathConfig.setTaus(.1);
  komo.add_qControlObjective({}, komo.k_order, 1e0);

  // homing
  if(qHome.N) komo.addObjective({}, FS_qItself, {}, OT_sos, {.1}, qHome);

//  komo.addObjective({}, FS_accumulatedCollisions, {}, OT_eq, {1e1});

  arr boxSize={.06,.15,.09};
  rai::ArgWord dir = rai::_xAxis;
  arr xLine, yzPlane;
  FeatureSymbol xyScalarProduct=FS_none, xzScalarProduct=FS_none;
  getGraspLinePlane(xLine, yzPlane, xyScalarProduct, xzScalarProduct, dir);

  //position: center in inner target plane; X-specific
  komo.addObjective({1.,2.}, FS_positionRel, {gripperName, boxName}, OT_eq, xLine*1e2, {});
  komo.addObjective({2.}, FS_positionRel, {gripperName, boxName}, OT_ineq, yzPlane*1e2, (boxSize/2.-.02));
  komo.addObjective({2.}, FS_positionRel, {gripperName, boxName}, OT_ineq, yzPlane*(-1e2), -(boxSize/2.-.02));

  //orientation: grasp axis orthoginal to target plane; X-specific
  komo.addObjective({1.,2.}, xyScalarProduct, {gripperName, boxName}, OT_eq, {1e2}, {});
  komo.addObjective({1.,2.}, xzScalarProduct, {gripperName, boxName}, OT_eq, {1e2}, {});

//  //no collision with palm
//  komo.addObjective({1.,2.}, FS_distance, {palmName, boxName}, OT_ineq, {1e1}, {-.001});

  komo.addObjective({1.}, FS_distance, {gripperName, boxName}, OT_eq, {1e1}, {-.1});

  komo.reset();
  komo.initWithConstant(q0);
  path = komo.getPath_qOrg();
  this->tau = komo.getPath_tau();

//  //optimize
//  uint trial=1;
//  komo.optimize(.01*trial, OptOptions().set_stopTolerance(1e-3)); //trial=0 -> no noise!

//  //is feasible?
//  bool feasible=komo.sos<50. && komo.ineq<.1 && komo.eq<.1;
//  cout <<"  seq  trial " <<trial <<(feasible?" good":" FAIL") <<" -- time:" <<komo.timeTotal <<"\t sos:" <<komo.sos <<"\t ineq:" <<komo.ineq <<"\t eq:" <<komo.eq <<endl;

//  komo.view(true);
//  while(komo.view_play(true, 1.));

//  arr path = komo.getPath_qOrg();

//  bot.move(path, {2., 2.5});
//  while(bot.step(C, .5));
//  if(bot.keypressed=='q' || bot.keypressed==27) return;

//  rai::wait();

}

void ChainMPC::reinit(const arr& x, const arr& v){
  //set the prefix to init:
  komo.setConfiguration_qOrg(-1, x);
  if(komo.k_order==2){
    komo.setConfiguration_qOrg(-2, x - komo.tau*v);
  }
  //initialize x(0) also to current
  //komo.setConfiguration_qOrg(0, x);
  //leave the leap configuration as is...
//  komo.timeSlices(-1,0)->setJointState({100.}); //this should be the tau joint!
}

void ChainMPC::reinit(const rai::Configuration& C){
  //shifts only prefix, not the whole trajectory! (would not make sense for x(H) \gets x(T) )
  komo.updateRootObjects(C);
}

void ChainMPC::solve(){
  //re-run KOMO
  rai::OptOptions opt;
  opt.stopTolerance = 1e-4;
  opt.stopGTolerance = 1e-4;
  komo.opt.verbose=0;
  komo.timeTotal=0.;
  komo.pathConfig.setJointStateCount=0;
  //komo.reportProblem();
  komo.optimize(0., opt);
  //komo.checkGradients();

  //is feasible?
  feasible=komo.sos<50. && komo.ineq<.1 && komo.eq<.1;
  rai::String msg;
  msg <<"it " <<steps <<" feasible: " <<(feasible?" good":" FAIL") <<" -- queries: " <<komo.pathConfig.setJointStateCount <<" time:" <<komo.timeTotal <<"\t sos:" <<komo.sos <<"\t ineq:" <<komo.ineq <<"\t eq:" <<komo.eq <<endl;

  komo.view(false, msg);

  path = komo.getPath_qOrg();
  this->tau = komo.getPath_tau();

//  while(komo.view_play(true, 1.));

//  arr path = komo.getPath_qOrg();

//  bot.move(path, {2., 2.5});
//  while(bot.step(C, .5));
//  if(bot.keypressed=='q' || bot.keypressed==27) return;

//  rai::wait();



  //store as output result
//  x1 = komo.getConfiguration_qOrg(0);
//  xT = komo.getConfiguration_qOrg(komo.T-1); //the last one
//  tau = komo.getPath_tau();
}

//===========================================================================

rai::Frame& setupWorld(rai::Configuration& C){
  C.addFile(rai::raiPath("../rai-robotModels/scenarios/pandasTable.g"));

  rai::Frame& target =
      C.addFrame("target")
      ->setShape(rai::ST_ssBox, {.06,.15,.09,.01})
      .setPosition(arr{-.3,-.2,.695});

  C.addFrame("plate", "table")
      ->setJoint(rai::JT_rigid)
      .setShape(rai::ST_ssBox, {.4,.4,.1,.01})
      .setRelativePosition(arr{-.4,.2,.0});

  return target;
}

//===========================================================================

void testPnp2() {
  rai::Configuration C;
  rai::Frame& target = setupWorld(C);
  arr center = C["l_gripper"]->getPosition();
  arr qHome = C.getJointState();

  //-- define constraints
  uint K=2;

  const char* gripperName="l_gripper";
  const char* palmName="l_palm";
  const char* boxName="target";
  const char* tableName="plate";
  const char* arm1Name="l_panda_coll7";
  const char* arm2Name="l_panda_coll6";

  rai::Array<ObjectiveL> phiflag(K);
  rai::Array<ObjectiveL> phirun(K);



  arr boxSize={.06,.15,.09};
  rai::ArgWord dir = rai::_xAxis;
  arr xLine, yzPlane;
  FeatureSymbol xyScalarProduct=FS_none, xzScalarProduct=FS_none;
  getGraspLinePlane(xLine, yzPlane, xyScalarProduct, xzScalarProduct, dir);

  //fixed pre-grasp distance
  phiflag(0).add(FS_distance, C, {gripperName, boxName}, OT_eq, {1e1}, {-.1});

  //plane centered
  phirun(1).add(FS_positionRel, C, {gripperName, boxName}, OT_eq, xLine*1e2, arr{});

  //orientation: grasp axis orthoginal to target plane; X-specific
  phirun(1).add(xyScalarProduct, C, {gripperName, boxName}, OT_eq, {1e2}, {});
  phirun(1).add(xzScalarProduct, C, {gripperName, boxName}, OT_eq, {1e2}, {});

  //within box
//  phiflag(1), FS_positionDiff, C, {gripperName, boxName}, OT_eq, {1e2}, {});
  phiflag(1).add(FS_positionRel, C, {gripperName, boxName}, OT_ineq, yzPlane*1e2, (boxSize/2.-.02));
  phiflag(1).add(FS_positionRel, C, {gripperName, boxName}, OT_ineq, yzPlane*(-1e2), -(boxSize/2.-.02));


  phirun(0).add(FS_distance, C, {palmName, boxName}, OT_ineq, {1e1}, {});

  //no collision with palm
//  phirun(1).add(FS_distance, C, {palmName, boxName}, OT_ineq, {1e1}, {-.001});


  //-- MPC stuff
  ChainMPC pathProblem;
  pathProblem.buildKOMO(C, phiflag, phirun, qHome);
  pathProblem.solve();

  FlagHuntingControl F(pathProblem.path, 1e0);
  F.tangents = zeros(K-1, qHome.N);
  F.tangents[-1] = pathProblem.path[-1] - pathProblem.path[-0];
  op_normalize(F.tangents[-1]());

  //pathProblem.solve();  rai::wait();  return;

  //-- start a robot thread
  BotOp bot(C, rai::checkParameter<bool>("real"));
  bot.home(C);
  double ctrlTime = 0., ctrlTimeLast;
  bot.setControllerWriteData(2);

  //-- iterate
  Metronome tic(1.);
  for(uint t=0;t<100;t++){
//    rai::wait(.1);
    tic.waitForTic();

    //-- switch target randomly at some times
    if(!(t%100)){
      F.phase=0;
      F.tau = 10.;
      ctrlTimeLast = bot.get_t();

      switch(rnd(4)){
        case 0: target.setPosition(center + arr{+.3,.0,+.2}); break;
        case 1: target.setPosition(center + arr{-.3,.0,+.2}); break;
        case 2: target.setPosition(center + arr{+.3,.0,-.2}); break;
        case 3: target.setPosition(center + arr{-.3,.0,-.2}); break;
      }
      LOG(0) <<"new target";
    }

    //-- get current state (time,q,qDot)
    ctrlTimeLast = ctrlTime;
    arr q,qDot;
    bot.getState(q, qDot, ctrlTime);

    rai::String msg;
    msg <<"CYCLE ctrlTime:" <<ctrlTime; //<<' ' <<q;

    //-- solve the pathProblem
    {
      pathProblem.reinit(C); //adopt all frames in C as prefix (also positions of objects)
//      pathProblem.reinit(q, qDot);
      pathProblem.solve();

      msg <<" (path) #:" <<pathProblem.komo.pathConfig.setJointStateCount;
//      msg <<" T:" <<pathProblem.komo.timeTotal <<" f:" <<pathProblem.komo.sos <<" eq:" <<pathProblem.komo.eq <<" iq:" <<pathProblem.komo.ineq;
    }

    //-- flag hunting update: update path
    F.update_flags(pathProblem.path);

    //-- flag hunting update: progress time (potentially phase) of
    if(!F.done()) F.update_progressTime(ctrlTime - ctrlTimeLast);

    //-- flag hunting update: phase backtracking
//    if(F.done()){
//      if(phiflag.elem(-1).maxError(C) > 1e-2) F.update_backtrack();
//    }
//    if(!F.done()){
//      while(phirun.elem(F.phase).maxError(C) > 1.) F.update_backtrack();
//    }

    //-- solve the timing problem
    if(!F.done()){
      auto ret = F.solve(q, qDot, 0);
      msg <<" (timing) ph:" <<F.phase <<" #:" <<ret->evals;
//      msg <<" T:" <<ret->time <<" f:" <<ret->f;
    }

    msg <<" tau: " <<F.tau << ctrlTime + F.getTimes(); // <<' ' <<F.vels;
    msg <<" (fea) "
       <<phirun.elem(0).maxError(C, 0) <<' '
       <<phiflag.elem(0).maxError(C, 0) <<' '
      <<phirun.elem(1).maxError(C, 0) <<' '
     <<phiflag.elem(1).maxError(C, 0);
    cout <<msg <<endl;

//    isFeasible(phiflag(0), C, 1e-2, 0);
//    isFeasible(phiflag(1), C, 1e-2, 0);

//    rai::Graph R = mpc.komo.getReport();

    //send leap target
    if(pathProblem.feasible){
//      bot.moveOverride(mpc.path, integral(mpc.tau) + .2);
//      if(t==15) bot.move(flags, integral(tau));

      if(!F.done()){
        arr times = F.getTimes();
        double gap = bot.get_t() - ctrlTime;
        times -= gap;
        bot.move(F.getFlags(), F.getVels(), F.getTimes(), true);
      }
    }
//    bot.moveAutoTimed(mpc.path);

    //update C
    bot.step(C, .0);
    if(bot.keypressed==13){ t=9; continue; }
    if(bot.keypressed=='q' || bot.keypressed==27) return;
  }
}

//===========================================================================

int main(int argc, char * argv[]){
  rai::initCmdLine(argc, argv);

//  rnd.clockSeed();
  rnd.seed(1);

  testPnp2();

  LOG(0) <<" === bye bye ===\n used parameters:\n" <<rai::getParameters()() <<'\n';

  return 0;
}
