#include "lgpop.h"

#include <iomanip>

#include <Gui/viewer.h>

#include <Kin/cameraview.h>
#include <Kin/kinViewer.h>

#include <Franka/controlEmulator.h>
#include <Franka/franka.h>
#include <Franka/gripper.h>
#include <Franka/help.h>

#include <RealSense/RealSenseThread.h>

#include <Perception/perceptViewer.h>
#include <Perception/perceptSyncer.h>

#include <BackgroundSubtraction/cv_depth_backgroundSubstraction.h>

LGPop::LGPop(bool _simulationMode)
  : simulationMode(_simulationMode){

  rawModel.addFile("../../model/pandaStation.g");
  rawModel.optimizeTree();
  q_home = rawModel.getJointState();

  ctrl_config.set() = rawModel;

  cam_pose.set() = rawModel["camera"]->X.getArr7d();

  {
    auto set = ctrl_state.set();
    rawModel.getJointState(set->q, set->qdot);
  }

}

LGPop::~LGPop(){
  reportCycleTimes();
}

void LGPop::runRobotControllers(){
  if(!simulationMode){
    ptr<Thread> F_right = make_shared<FrankaThread>(ctrl_ref, ctrl_state, 0, franka_getJointIndices(rawModel,'R'));
    ptr<Thread> F_left =  make_shared<FrankaThread>(ctrl_ref, ctrl_state, 1, franka_getJointIndices(rawModel,'L'));
    ptr<Thread> G_right = make_shared<FrankaGripper>(0);
    ptr<Thread> G_left =  make_shared<FrankaGripper>(1);
    processes.append({F_right, F_left, G_right, G_left});
  }else{
    ptr<Thread> F_emul = make_shared<ControlEmulator>(ctrl_ref, ctrl_state, rawModel);
    ptr<Thread> G_emul = make_shared<GripperEmulator>();
    processes.append({F_emul, G_emul});
  }
}

void LGPop::runTaskController(int verbose){
  ptr<Thread> TC = make_shared<TaskControlThread>(ctrl_config, ctrl_ref, ctrl_state, ctrl_tasks);
  processes.append(TC);
  if(verbose){
    ptr<Thread> view = make_shared<KinViewer>(ctrl_config, .1);
    processes.append(view);
  }
}

void LGPop::runCamera(int verbose){
  if(!simulationMode){
    auto cam = make_shared<RealSenseThread>(cam_color, cam_depth);
    cam_depth.waitForNextRevision();
    cam_Fxypxy.set() = cam->depth_fxypxy;
    processes.append(std::dynamic_pointer_cast<Thread>(cam));
  }else{
    auto cam = make_shared<rai::Sim_CameraView>(ctrl_config, cam_color, cam_depth, .05, "camera");
    auto sen = cam->C.currentSensor;
    cam_Fxypxy.set() = ARR(sen->cam.focalLength*sen->height, sen->cam.focalLength*sen->height, .5*sen->width, .5*sen->height);
    processes.append(std::dynamic_pointer_cast<Thread>(cam));
  }
  if(verbose){
    cam_color.name() = "cam_color";
    ptr<Thread> view1 = make_shared<ImageViewer>(cam_color, .1);
    cam_depth.name() = "cam_depth";
    ptr<Thread> view2 = make_shared<ImageViewerFloat>(cam_depth, .1, 128.f);
    processes.append({view1, view2});
  }
}

void LGPop::runPerception(int verbose){
  //-- compute model view with robot mask and depth
  byteA frameIDmap = franka_getFrameMaskMap(rawModel);
  ptr<Thread> masker = make_shared<rai::Sim_CameraView>(ctrl_config, model_segments, model_depth,
                                                        .05, "camera", true, frameIDmap);
  model_segments.name()="model_segments";
  ptr<Thread> viewer = make_shared<ImageViewer>(model_segments);
  processes.append({masker, viewer});

  //-- big OpenCV process that generates basic percepts
  ptr<Thread> opencv =
      make_shared<CV_BackgroundSubstraction_Thread>(percepts, cam_color, cam_depth, model_segments, cam_pose, cam_Fxypxy, verbose);
  opencv->name="name";
  processes.append(opencv);

  //-- percept filter and integration in model
  ptr<Thread> filter = make_shared<SyncFiltered> (percepts, ctrl_config);
  filter->name="syncer";
  if(verbose){
    ptr<Thread> view = make_shared<PerceptViewer>(percepts, ctrl_config);
    ptr<Thread> view2 = make_shared<KinViewer>(ctrl_config);
    processes.append({filter, view, view2});
  }
}

void LGPop::pauseProcess(const char* name, bool resume){
  for(ptr<Thread>& thread: processes) {
    if(thread->name==name){
      if(!resume) thread->threadStop();
    }
  }
}

void LGPop::reportCycleTimes(){
  cout <<"Cycle times for all Threads (msec):" <<endl;
  for(ptr<Thread>& thread: processes) {
    cout <<std::setw(30) <<thread->name <<" : " <<thread->timer.report() <<endl;
  }
}