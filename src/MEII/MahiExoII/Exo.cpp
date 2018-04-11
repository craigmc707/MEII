#include <MEII/MahiExoII/Exo.hpp>

using namespace mel;

namespace meii {

    Exo::Exo(const std::string& name) :
        Robot(name)
    { }

    std::vector<double> Exo::get_anatomical_joint_positions() const {
        return anatomical_joint_positions_;
    }

    double Exo::get_anatomical_joint_position(int index) const {
        return anatomical_joint_positions_[index];
    }

    std::vector<double> Exo::get_anatomical_joint_velocities() const {
        return anatomical_joint_velocities_;
    }

    double Exo::get_anatomical_joint_velocity(int index) const {
        return anatomical_joint_velocities_[index];
    }

}