/*  ------------------------------------------------------------------
    Copyright (c) 2011-2020 Marc Toussaint
    email: toussaint@tu-berlin.de

    This code is distributed under the MIT License.
    Please see <root-path>/LICENSE for details.
    --------------------------------------------------------------  */

#ifdef RAI_PYBIND

#include "pyBot.h"

#include <ry/types.h>

#include "bot.h"

#include <KOMO/pathTools.h>

//PYBIND11_MODULE(libpybot, m) {
//  m.doc() = "bot bindings";

//  init_PyBot(m);
//}

void init_BotOp(pybind11::module& m) {
  
  pybind11::class_<BotOp, shared_ptr<BotOp>>(m, "BotOp", "")

  .def(pybind11::init<rai::Configuration&, bool>(), "")

  .def("get_t", &BotOp::get_t,
       "returns the control time (absolute time managed by the high freq tracking controller)")

  .def("get_qHome", &BotOp::get_qHome,
       "returns home joint vector (defined as the configuration C when you created BotOp)")

  .def("get_q", &BotOp::get_q,
       "get the current (real) robot joint vector")

  .def("get_qDot", &BotOp::get_qDot,
       "get the current (real) robot joint velocities")

  .def("getTimeToEnd", &BotOp::getTimeToEnd,
       "get time-to-go of the current spline reference that is tracked (use getTimeToEnd()<=0. to check if motion execution is done)")

  //.def("move", pybind11::overload_cast<const arr&, const arr&, const arr&, bool, double>(&BotOp::move))
  .def("move", pybind11::overload_cast<const arr&, const arr&, bool, double>(&BotOp::move),
       "core motion command: set a spline motion reference; if only a single time [T] is given for multiple waypoints, it assumes equal time spacing with TOTAL time T"
       "\n\nBy default, the given spline is APPENDED to the current reference spline. The user can also enforce the given spline to overwrite the current reference "
       "starting at the given absolute ctrlTime. This allows implementation of reactive (e.g. MPC-style) control. However, the user needs to take care that "
       "overwriting is done in a smooth way, i.e., that the given spline starts with a pos/vel that is close to the pos/vel of the current reference at the given ctrlTime.",
       pybind11::arg("path"),
       pybind11::arg("times"),
       pybind11::arg("overwrite") = false,
       pybind11::arg("overwriteCtrlTime") = -1.)

  .def("moveAutoTimed", &BotOp::moveAutoTimed,
       "helper to execute a path (typically fine resolution, from KOMO or RRT) with equal time spacing chosen for given max vel/acc",
       pybind11::arg("path"),
       pybind11::arg("maxVel") =  1.,
       pybind11::arg("maxAcc") =  1.)

  .def("moveTo", &BotOp::moveTo,
       "helper to move to a single joint vector target, where timing is chosen optimally based on the given timing cost"
       "\n\nWhen using overwrite, this immediately steers to the target -- use this as a well-timed reactive q_target controller",
       pybind11::arg("q_target"),
       pybind11::arg("timeCost") = 1.,
       pybind11::arg("overwrite") = false)

  .def("setControllerWriteData", &BotOp::setControllerWriteData,
       "[for internal debugging only] triggers writing control data into a file")

  .def("gripperOpen", &BotOp::gripperOpen,
       "open gripper",
      pybind11::arg("leftRight"),
      pybind11::arg("width") = .075,
      pybind11::arg("speed") = .075)

  .def("gripperClose", &BotOp::gripperClose,
       "close gripper",
       pybind11::arg("leftRight"),
       pybind11::arg("force") = 10.,
       pybind11::arg("width") = .05,
       pybind11::arg("speed") = .1)

   .def("gripperCloseGrasp", &BotOp::gripperCloseGrasp,
        "close gripper and indicate what should be grasped -- makes no different in real, but helps simulation to mimic grasping more reliably",
        pybind11::arg("leftRight"),
        pybind11::arg("objName"),
        pybind11::arg("force") = 10.,
        pybind11::arg("width") = .05,
        pybind11::arg("speed") = .1)

  .def("gripperPos", &BotOp::gripperPos,
       "returns the gripper pos",
       pybind11::arg("leftRight"))

  .def("gripperDone", &BotOp::gripperDone,
       "returns if gripper is done",
       pybind11::arg("leftRight"))

  .def("sync", &BotOp::sync,
       "sync your workspace configuration C with the robot state",
       pybind11::arg("C"),
       pybind11::arg("waitTime") = .1)

  .def("home", &BotOp::home,
       "drive the robot home (which is defined as the configuration C when you created BotOp); keeps argument C synced",
       pybind11::arg("C"))

  .def("hold", &BotOp::hold,
       "hold the robot with a trivial PD controller, floating means reference = real, without damping the robot is free floating",
       pybind11::arg("floating") = false,
       pybind11::arg("damping") = true)
  ;

  //===========================

  m.def("getStartGoalPath", [](rai::Configuration& C, const arr& qTarget, const arr& qHome){
      return getStartGoalPath(C, qTarget, qHome);
  } );

}

#endif
