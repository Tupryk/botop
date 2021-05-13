#Include = '../../data/baxter_model/baxter.ors'
Include='../rai-robotModels/kuka_drake/setup.g'

Edit base { X=<T t(0 0 .7)> } # for kuka
#Edit base_footprint { X=<T t(-.4 0 0)> } # for baxter

#body slider1a { type=box size=[.2 .02 .02 0] color=[.5 .5 .5] }
#body slider1b { type=box size=[.2 .02 .02 0] color=[.8 .3 .3] }
#joint slider1Joint(slider1a slider1b){ type=transX }
#shape (slider1b){ rel=<T t(.1 0 0)> type=5 size=[.1 .1 .1] color=[0 1 0] }


Edit endeff { logical={ gripper, free } }

