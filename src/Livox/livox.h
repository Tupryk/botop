#pragma once

#include <Kin/kin.h>
#include <Core/thread.h>


struct Point {
    float x;
    float y;
    float z;
};

struct PointCloudData {
    std::vector<Point> points;
    int max_points;
};

namespace rai {

    struct Livox : Thread {

        int max_points;
        arr points;
        PointCloudData points_message;

        RAI_PARAM("livox/", double, filter, .9)

        Livox();
        ~Livox();

        void pull(rai::Configuration& C);

        void step();

        private:
            std::mutex mux;
            std::map<std::string, rai::Transformation> poses;
    };

} //namespace
