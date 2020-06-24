#include <MEII/Control/DynamicMotionPrimitive.hpp>
#include <MEII/Control/MinimumJerk.hpp>
#include <MEII/Control/Trajectory.hpp>
#include <MEII/MahiExoII/MahiExoII.hpp>
#include <Mahi/Com.hpp>
#include <Mahi/Util.hpp>
#include <Mahi/Daq.hpp>
#include <Mahi/Robo.hpp>
#include <Mahi/Com.hpp>
#include <vector>

using namespace mahi::util;
using namespace mahi::daq;
using namespace mahi::robo;
using namespace mahi::com;
using namespace meii;

enum state {
    to_neutral_0,     // 0
    to_bottom_elbow,  // 1
    to_top_elbow,     // 2
    to_neutral_1,     // 3
    to_top_wrist,     // 4
    wrist_circle,     // 5
    to_neutral_2      // 6
};

// create global stop variable CTRL-C handler function
ctrl_bool stop(false);
bool handler(CtrlEvent event) {
    stop = true;
    return true;
}

void to_state(state& current_state_, const state next_state_, WayPoint current_position_, WayPoint new_position_, Time traj_length_, DynamicMotionPrimitive& dmp_, Clock ref_traj_clock_) {
    current_position_.set_time(seconds(0));
    new_position_.set_time(traj_length_);
    dmp_.set_endpoints(current_position_, new_position_);
    if (!dmp_.trajectory().validate()) {
        LOG(Warning) << "DMP trajectory invalid.";
        stop = true;
    }
    current_state_ = next_state_;
    ref_traj_clock_.restart();
}

int main(int argc, char* argv[]) {
    // register ctrl-c handler
    register_ctrl_handler(handler);

    // make options
    Options options("ex_pos_control_nathan.exe", "Nathan's Position Control Demo");
    options.add_options()
		("c,calibrate", "Calibrates the MAHI Exo-II")
		("m,multi", "MAHI Exo-II follows a multi-DoF trajectory generated by dmps")
        ("n,no_torque", "trajectories are generated, but not torque provided")
		("h,help", "Prints this help message");

    auto result = options.parse(argc, argv);

    // if -h, print the help option
    if (result.count("help") > 0) {
        print_var(options.help());
        return 0;
    }

    // enable Windows realtime
    enable_realtime();

    /////////////////////////////////
    // construct Q8 USB and configure
    /////////////////////////////////
    Q8Usb q8;
    q8.open();

    std::vector<TTL> idle_values(8,TTL_HIGH);
    q8.DO.enable_values.set({0,1,2,3,4,5,6,7},idle_values);
    q8.DO.disable_values.set({0,1,2,3,4,5,6,7},idle_values);
    q8.DO.expire_values.write({0,1,2,3,4,5,6,7},idle_values);

    Time Ts = milliseconds(1);  // sample period for DAQ

    ////////////////////////////////

    //////////////////////////////////////////////
    // create MahiExoII and bind Q8 channels to it
    //////////////////////////////////////////////


    MeiiConfiguration config(q8, // daq q8
                             {1, 2, 3, 4, 5}, // encoder channels (encoder)
                             {1, 2, 3, 4, 5}, // enable channels (DO)
                             {1, 2, 3, 4, 5}, // current write channels (AO)
                             {TTL_LOW, TTL_LOW, TTL_LOW, TTL_LOW, TTL_LOW}, // enable values for motors
                             {1.8, 1.8, 0.184, 0.184, 0.184}); // amplifier gains (A/V)
    MahiExoII meii(config);

    bool rps_is_init = false;

    //////////////////////////////////////////////

    // calibrate - manually zero the encoders (right arm supinated)
    if (result.count("calibrate") > 0) {
        meii.calibrate_auto(stop);
        LOG(Info) << "MAHI Exo-II encoders calibrated.";
        return 0;
    }

    // make MelShares
    MelShare ms_pos("ms_pos");
    MelShare ms_vel("ms_vel");
    MelShare ms_trq("ms_trq");
    MelShare ms_ref("ms_ref");

    // create ranges for saturating trajectories for safety  MIN            MAX
    std::vector<std::vector<double>> setpoint_rad_ranges = {{-90 * DEG2RAD, 0 * DEG2RAD},
                                                            {-90 * DEG2RAD, 90 * DEG2RAD},
                                                            {-15 * DEG2RAD, 15 * DEG2RAD},
                                                            {-15 * DEG2RAD, 15 * DEG2RAD},
                                                            {0.08, 0.115}};

                                     // state 0    // state 1    // state 2    // state 3    // state 4    // state 5    // state 6
    std::vector<Time> state_times = {seconds(2.0), seconds(2.0), seconds(4.0), seconds(2.0), seconds(1.0), seconds(4.0), seconds(1.0)};

    std::vector<double> ref;

    // setup trajectories

    Time dmp_Ts = milliseconds(50);

    // waypoints                                   Elbow F/E       Forearm P/S   Wrist F/E     Wrist R/U     LastDoF
    WayPoint neutral_point = WayPoint(Time::Zero, {-35 * DEG2RAD,  00 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD, 0.09});
    WayPoint bottom_elbow  = WayPoint(Time::Zero, {-65 * DEG2RAD,  30 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD, 0.09});
    WayPoint top_elbow     = WayPoint(Time::Zero, { -5 * DEG2RAD, -30 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD, 0.09});
    WayPoint top_wrist     = WayPoint(Time::Zero, {-35 * DEG2RAD,  00 * DEG2RAD, 00  * DEG2RAD, 15 * DEG2RAD, 0.09});

    // construct timer in hybrid mode to avoid using 100% CPU
    Timer timer(Ts, Timer::Hybrid);
    timer.set_acceptable_miss_rate(0.05);

    // construct clock for regulating keypress
    Clock keypress_refract_clock;
    Time keypress_refract_time = seconds(0.5);

    std::vector<std::string> dof_str = {"ElbowFE", "WristPS", "WristFE", "WristRU"};

    ////////////////////////////////////////////////
    //////////// State Manager Setup ///////////////
    ////////////////////////////////////////////////

    state current_state = to_neutral_0;
    WayPoint current_position;
    WayPoint new_position;
    Time traj_length;
    DynamicMotionPrimitive dmp(dmp_Ts, neutral_point, neutral_point.set_time(state_times[to_neutral_0]));
	std::vector<double> traj_max_diff = { 50 * DEG2RAD, 50 * DEG2RAD, 25 * DEG2RAD, 25 * DEG2RAD, 0.1 };
	dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);
    Clock ref_traj_clock;

    std::vector<double> aj_positions;
    std::vector<double> aj_velocities;

    std::vector<double> command_torques;
    std::vector<double> rps_command_torques;

    ref_traj_clock.restart();
	
	// enable DAQ and exo
	q8.enable();
	meii.enable();
	
	q8.watchdog.start();

    // trajectory following
    if (result.count("single") > 0 || result.count("no_torque") > 0) {
        LOG(Info) << "Starting Movement.";

        WayPoint start_pos(Time::Zero, meii.get_anatomical_joint_positions());

        dmp.set_endpoints(start_pos, neutral_point.set_time(state_times[to_neutral_0]));

        while (!stop) {
            // update all DAQ input channels
            q8.read_all();

            // update MahiExoII kinematics
            meii.update_kinematics();

            for (int i = 0; i < meii.n_aj; ++i) {
                aj_positions[i] = meii.get_anatomical_joint_position(i);
                aj_velocities[i] = meii.get_anatomical_joint_velocity(i);
            }

            if (current_state != wrist_circle) {
                // update reference from trajectory
                ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());
            } 
			else {
                ref[0] = neutral_point.get_pos()[0];
                ref[1] = neutral_point.get_pos()[1];
                ref[2] = 15.0 * DEG2RAD * sin(2.0 * PI * ref_traj_clock.get_elapsed_time() / state_times[wrist_circle]);
                ref[3] = 15.0 * DEG2RAD * cos(2.0 * PI * ref_traj_clock.get_elapsed_time() / state_times[wrist_circle]);
                ref[4] = neutral_point.get_pos()[4];
            }

            // constrain trajectory to be within range
            for (std::size_t i = 0; i < meii.n_aj; ++i) {
                ref[i] = clamp(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
            }

            // calculate anatomical command torques
            command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], aj_positions[0], 0, meii.meii_joints[0]->get_velocity());
            command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], aj_positions[1], 0, meii.meii_joints[1]->get_velocity());
            for (std::size_t i = 0; i < meii.n_qs; ++i) {
                rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], aj_positions[i + 2], 0, aj_velocities[i + 2]);
            }
            std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

            if (result.count("no_torque") > 0){
                command_torques = {0.0, 0.0, 0.0, 0.0, 0.0};
            }
            
            // set anatomical command torques
            meii.set_anatomical_raw_joint_torques(command_torques);

            // if enough time has passed, continue to the next state. See to_state function at top of file for details
            if (ref_traj_clock.get_elapsed_time() > state_times[current_state]) {
                switch (current_state) {
                    case to_neutral_0:
                        to_state(current_state, to_bottom_elbow, current_position.set_pos(aj_positions), bottom_elbow, state_times[to_bottom_elbow], dmp, ref_traj_clock);
                        break;
                    case to_bottom_elbow:
                        to_state(current_state, to_top_elbow, current_position.set_pos(aj_positions), bottom_elbow, state_times[to_top_elbow], dmp, ref_traj_clock);
                        break;
                    case to_top_elbow:
                        to_state(current_state, to_neutral_1, current_position.set_pos(aj_positions), bottom_elbow, state_times[to_neutral_1], dmp, ref_traj_clock);
                        break;
                    case to_neutral_1:
                        to_state(current_state, to_top_wrist, current_position.set_pos(aj_positions), bottom_elbow, state_times[to_top_wrist], dmp, ref_traj_clock);
                        break;
                    case to_top_wrist:
                        to_state(current_state, wrist_circle, current_position.set_pos(aj_positions), bottom_elbow, state_times[wrist_circle], dmp, ref_traj_clock);
                        break;
                    case wrist_circle:
                        to_state(current_state, to_neutral_2, current_position.set_pos(aj_positions), bottom_elbow, state_times[to_neutral_2], dmp, ref_traj_clock);
                        break;
                    case to_neutral_2:
                        stop = true;
                        break;
                }
            }

            // update all DAQ output channels
            q8.write_all();

            // kick watchdog
            if (!q8.watchdog.kick() || meii.any_limit_exceeded()) {
                stop = true;
            }

            // wait for remainder of sample period
            timer.wait();
        }

        meii.disable();
        q8.disable();
    }

    // clear console buffer
    while (get_key_nb() != 0);

    disable_realtime();

    return 0;
}