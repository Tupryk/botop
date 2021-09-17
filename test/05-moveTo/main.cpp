#include <Franka/controlEmulator.h>
#include <Franka/franka.h>
#include <Franka/help.h>
#include <Algo/SplineCtrlFeed.h>

const char *USAGE =
    "\nTest of low-level (without bot interface) of SplineCtrlReference"
    "\n";

//===========================================================================

void test() {
  //-- setup a configuration
  rai::Configuration C;
  C.addFile(rai::raiPath("../rai-robotModels/scenarios/pandaSingle.g"));
  C.watch(true);

  //-- start a robot thread
  C.ensure_indexedJoints();
  std::shared_ptr<rai::RobotAbstraction> robot;
  if(rai::getParameter<bool>("real", false)){
    robot = make_shared<FrankaThreadNew>(0, franka_getJointIndices(C,'l'));
  }else{
    robot = make_shared<ControlEmulator>(C);
  }
  robot->writeData = true;

  //-- create 2 simple reference configurations
  arr q0 = robot->state.get()->q;
  arr qT = q0;
  qT(1) -= .5;

  //-- define the reference feed to be a spline
  auto sp = make_shared<rai::SplineCtrlReference>();
  robot->cmd.set()->ref = sp;

  //1st motion:
  double ctrlTime = robot->state.get()->time;
  sp->append(cat(qT, qT, q0).reshape(3,-1), arr{2., 2., 4.}, ctrlTime, true);

  for(;;){
    if(C.watch(false,STRING("time: "<<robot->state.get()->time))=='q') break;
    C.setJointState(robot->state.get()->q);
    rai::wait(.1);
  }

  //2nd motion:
  ctrlTime = robot->state.get()->time;
  sp->moveTo(qT, 1., ctrlTime, false);
  cout <<"OVERRIDE AT t=" <<ctrlTime <<endl;
  sp->moveTo(q0, 1., ctrlTime, true);

  for(;;){
    if(C.watch(false,STRING("time: "<<robot->state.get()->time))=='q') break;
    C.setJointState(robot->state.get()->q);
    rai::wait(.1);
  }

}

//===========================================================================

int main(int argc, char * argv[]){
  rai::initCmdLine(argc, argv);

  cout <<USAGE <<endl;

  test();

  LOG(0) <<" === bye bye ===\n used parameters:\n" <<rai::getParameters()() <<'\n';

  return 0;
}
