#include <MEII/MahiExoII/MahiExoII.hpp>
#include <MEII/Control/Trajectory.hpp>
#include <Mahi/Daq.hpp>
#include <Mahi/Util.hpp>
#include <Mahi/Com.hpp>

using namespace meii;

using namespace mahi::daq;
using namespace mahi::util;
using namespace mahi::com;

// create global stop variable CTRL-C handler function
ctrl_bool stop(false);
bool handler(CtrlEvent event) {
    stop = true;
    return true;
}

int main(int argc, char *argv[]) {

    register_ctrl_handler(handler);

    // make options
    Options options("ex_mahiexoii_pos_ctrl.exe", "MahiExoII Position Control Demo");
    options.add_options()
        ("c,calibrate",  "Calibrates the MAHI Exo-II")
        ("s,setpoint",   "Runs the MAHI Exo-II MelScope setpoint control demo")
        ("t,trajectory", "Runs the MAHI Exo-II trajectory following demo")
        ("h,help", "Prints this help message");

    auto result = options.parse(argc, argv);

    if (result.count("help") > 0) {
        print_var(options.help());
        return 0;
    }

    // enable Windows realtime
    enable_realtime();

    // register ctrl-c handler
    register_ctrl_handler(handler);   

    // make Q8 USB and configure
    Q8Usb q8;

    std::vector<TTL> idle_values(8,TTL_HIGH);
    q8.DO.enable_values.set({0,1,2,3,4,5,6,7},idle_values);
    q8.DO.disable_values.set({0,1,2,3,4,5,6,7},idle_values);
    q8.DO.expire_values.write({0,1,2,3,4,5,6,7},idle_values);

    // create MahiExoII and bind Q8 channels to it
    MeiiConfiguration config(q8, // daq q8
                             {1, 2, 3, 4, 5}, // encoder channels (encoder)
                             {1, 2, 3, 4, 5}, // enable channels (DO)
                             {1, 2, 3, 4, 5}, // current write channels (AO)
                             {TTL_LOW, TTL_LOW, TTL_LOW, TTL_LOW, TTL_LOW}, // enable values for motors
                             {1.8, 1.8, 0.184, 0.184, 0.184}); // amplifier gains (A/V)
    MahiExoII meii(config);

    // calibrate - manually zero the encoders (right arm supinated)
    if (result.count("calibrate") > 0) {       
        meii.calibrate(stop);
        LOG(Info) << "MAHI Exo-II encoders calibrated.";
        return 0;
    }

    bool save_data = false;
    std::string filepath = "example_meii_robot_data_log.csv";

    // create robot data log
    // DataLogger robot_log(WriterType::Buffered, false);
    std::vector<double> robot_log_row(16);
    std::vector<std::vector<double>> robot_log;
    std::vector<std::string> header = { "Time [s]", "MEII EFE Position [rad]", "MEII EFE Velocity [rad/s]", "MEII EFE Commanded Torque [Nm]",
        "MEII FPS Position [rad]", "MEII FPS Velocity [rad/s]", "MEII FPS Commanded Torque [Nm]",
        "MEII RPS L1 Position [m]", "MEII RPS L1 Velocity [m/s]", "MEII RPS L1 Commanded Force [N]",
        "MEII RPS L2 Position [m]", "MEII RPS L2 Velocity [m/s]", "MEII RPS L2 Commanded Force [N]",
        "MEII RPS L3 Position [m]", "MEII RPS L3 Velocity [m/s]", "MEII RPS L3 Commanded Force [N]" };
    // robot_log.set_header(log_header);
    // robot_log.set_record_format(DataFormat::Default, 12);
    

    // make MelShares
    MelShare ms_pos("ms_pos");
    MelShare ms_vel("ms_vel");
    MelShare ms_trq("ms_trq");
    MelShare ms_sp("ms_sp");
    MelShare ms_ref("ms_ref");


    // setpoint control with MelScope
    if (result.count("setpoint") > 0) {
        LOG(Info) << "MAHI Exo-II Setpoint Control.";

        // create initial setpoint and ranges
        std::vector<double> setpoint_deg = { -35, 0, 0, 0, 0.10 };
        std::vector<double> setpoint_rad(meii.N_aj_);
        for (size_t i = 0; i < meii.N_aj_ - 1; ++i) {
            setpoint_rad[i] = setpoint_deg[i] * DEG2RAD;
        }
        std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
        { -90 * DEG2RAD, 90 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { 0.08, 0.115 } };
        ms_sp.write_data(setpoint_deg);

        // set up state machine
        std::size_t state = 0;
        Time backdrive_time = seconds(3);

        // create data containers
        std::vector<double> rj_positions(meii.N_rj_);
        std::vector<double> rj_velocities(meii.N_rj_);
        std::vector<double> aj_positions(meii.N_aj_);
        std::vector<double> aj_velocities(meii.N_aj_);
        std::vector<double> command_torques(meii.N_aj_, 0.0);
        std::vector<double> rps_command_torques(meii.N_qs_, 0.0);

        // enable DAQ and exo
        q8.enable();
        meii.enable();

        // initialize controller
        meii.set_rps_control_mode(0);

        // construct timer in hybrid mode to avoid using 100% CPU
        Timer timer(milliseconds(1), Timer::Hybrid);

        // start loop
        LOG(Info) << "Robot Backdrivable.";
        q8.watchdog.start();
        while (!stop) {

            // update all DAQ input channels
            q8.read_all();

            // update MahiExoII kinematics
            meii.update_kinematics();

            // store most recent readings from DAQ
            for (int i = 0; i < meii.N_rj_; ++i) {
                rj_positions[i] = meii.meii_joints[i].get_position();
                rj_velocities[i] = meii.meii_joints[i].get_velocity();
            }
            for (int i = 0; i < meii.N_aj_; ++i) {
                aj_positions[i] = meii.get_anatomical_joint_position(i);
                aj_velocities[i] = meii.get_anatomical_joint_velocity(i);
            }

            switch (state) {
            case 0: // backdrive

                // command zero torque
                meii.set_joint_torques(command_torques);

                // check for wait period to end
                if (timer.get_elapsed_time() >= backdrive_time) {
                    meii.rps_init_par_ref_.start(meii.get_wrist_parallel_positions(), timer.get_elapsed_time());
                    state = 1;
                    LOG(Info) << "Initializing RPS Mechanism.";
                }
                break;

            case 1: // initialize rps                

                // calculate commanded torques
                rps_command_torques = meii.set_rps_pos_ctrl_torques(meii.rps_init_par_ref_, timer.get_elapsed_time());
                std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

                // check for RPS Initialization target reached
                if (meii.check_rps_init()) {
                    LOG(Info) << "RPS initialization complete.";
                    meii.set_rps_control_mode(2); // platform height non-backdrivable
                    meii.anat_ref_.start(setpoint_rad, meii.get_anatomical_joint_positions(), timer.get_elapsed_time());
                    state = 2;
                }
                break;

            case 2: // setpoint control

                // read in setpoint from MelShare
                setpoint_deg = ms_sp.read_data();
                for (std::size_t i = 0; i < 4; ++i) {
                    setpoint_rad[i] = setpoint_deg[i] * DEG2RAD;
                }
                setpoint_rad[4] = setpoint_deg[4];


                // update and saturate setpoint
                for (int i = 0; i < meii.N_aj_; ++i) {
                    setpoint_rad[i] = clamp(setpoint_rad[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
                }
                meii.anat_ref_.set_ref(setpoint_rad, timer.get_elapsed_time());


                // calculate commanded torques
                command_torques = meii.set_anat_pos_ctrl_torques(meii.anat_ref_, timer.get_elapsed_time());


                break;
            }

            // write to MelShares
            ms_pos.write_data(aj_positions);
            ms_vel.write_data(aj_velocities);
            ms_trq.write_data(command_torques);

            // update all DAQ output channels
            q8.write_all();

            // write to robot data log
            robot_log_row[0] = timer.get_elapsed_time().as_seconds();
            for (std::size_t i = 0; i < meii.N_rj_; ++i) {
                robot_log_row[3 * i + 1] = meii[i].get_position();
                robot_log_row[3 * i + 2] = meii[i].get_velocity();
                robot_log_row[3 * i + 3] = meii[i].get_torque_command();
            }
            robot_log.push_back(robot_log_row);

            // check for save key
            // check for stop key
            int key_press = -1;
            key_press = get_key_nb();
            if (key_press == 13) {
                stop = true;
            }

            // kick watchdog
            if (!q8.watchdog.kick() || meii.any_limit_exceeded())
                stop = true;

            // wait for remainder of sample period
            timer.wait();

        }
    } // end setpoitn control with MelScop


    // trajectory following
    if (result.count("trajectory") > 0) {
        LOG(Info) << "MAHI Exo-II Trajectory Following.";

        // create ranges
        std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
        { -90 * DEG2RAD, 90 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { 0.08, 0.115 } };

        // create discrete trajectory at with certain max velocities
        std::vector<double> traj_max_diff = { 10 * DEG2RAD, 10 * DEG2RAD, 5 * DEG2RAD, 5 * DEG2RAD, 0.01 };
        //Trajectory::Point initial_waypoint(seconds(0), { -35 * DEG2RAD, 0, 0, 0, 0.10 });
        //Trajectory::Point final_waypoint(seconds(3), { -5 * DEG2RAD, 0, 0, 0, 0.10 });
        WayPoint initial_waypoint;
        WayPoint final_waypoint(seconds(5), { -35 * DEG2RAD, 20 * DEG2RAD, 15 * DEG2RAD, 0, 0.09 });
        std::vector<WayPoint> waypoints(2);
        waypoints[1] = final_waypoint;
        //waypoints.push_back(initial_waypoint);
        //waypoints.push_back(final_waypoint);
        //Trajectory ref_traj(MahiExoII::N_aj_, waypoints, Trajectory::Linear, traj_max_diff);
        Trajectory ref_traj;

        // construct for trajectory
        Clock ref_traj_clock;

        // set up state machine
        std::size_t state = 0;
        Time backdrive_time = seconds(3);

        // create data containers
        std::vector<double> rj_positions(meii.N_rj_);
        std::vector<double> rj_velocities(meii.N_rj_);
        std::vector<double> aj_positions(meii.N_aj_);
        std::vector<double> aj_velocities(meii.N_aj_);
        std::vector<double> command_torques(meii.N_aj_, 0.0);
        std::vector<double> rps_command_torques(meii.N_qs_, 0.0);
        std::vector<double> ref(meii.N_aj_, 0.0);

        // enable DAQ and exo
        q8.enable();
        meii.enable();

        // initialize controller
        meii.set_rps_control_mode(0);

        // construct timer in hybrid mode to avoid using 100% CPU
        Timer timer(milliseconds(1), Timer::Hybrid);

        // start loop
        LOG(Info) << "Robot Backdrivable.";
        q8.watchdog.start();
        while (!stop) {

            // update all DAQ input channels
            q8.read_all();

            // update MahiExoII kinematics
            meii.update_kinematics();

            // store most recent readings from DAQ
            for (int i = 0; i < meii.N_rj_; ++i) {
                rj_positions[i] = meii[i].get_position();
                rj_velocities[i] = meii[i].get_velocity();
            }
            for (int i = 0; i < meii.N_aj_; ++i) {
                aj_positions[i] = meii.get_anatomical_joint_position(i);
                aj_velocities[i] = meii.get_anatomical_joint_velocity(i);
            }

            switch (state) {
            case 0: // backdrive

                // update ref, though not being used
                ref = meii.get_anatomical_joint_positions();

                // command zero torque
                meii.set_joint_torques(command_torques);

                // check for wait period to end
                if (timer.get_elapsed_time() >= backdrive_time) {
                    meii.rps_init_par_ref_.start(meii.get_wrist_parallel_positions(), timer.get_elapsed_time());
                    state = 1;
                    LOG(Info) << "Initializing RPS Mechanism.";
                }
                break;

            case 1: // initialize rps                

                // update ref, though not being used
                ref = meii.get_anatomical_joint_positions();

                // calculate commanded torques
                rps_command_torques = meii.set_rps_pos_ctrl_torques(meii.rps_init_par_ref_, timer.get_elapsed_time());
                std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

                // check for RPS Initialization target reached
                if (meii.check_rps_init()) {
                    LOG(Info) << "RPS initialization complete.";
                    meii.set_rps_control_mode(2); // platform height backdrivable
                    //meii.anat_ref_.start(setpoint_rad, meii.get_anatomical_joint_positions(), timer.get_elapsed_time());
                    state = 2;
                    // command_torques = std::vector<double>(meii.N_aj_, 0.0);
                    meii.set_anatomical_joint_torques(command_torques);
                    initial_waypoint = WayPoint(seconds(0.0), aj_positions);
                    waypoints[0] = initial_waypoint;
                    ref_traj.set_waypoints(meii.N_aj_, waypoints, Trajectory::Interp::Linear, traj_max_diff);
                    ref_traj_clock.restart();
                }
                break;

            case 2: // trajectory following

                // update reference from trajectory
                ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());

                // constraint trajectory to be within range
                for (int i = 0; i < meii.N_aj_; ++i) {
                    ref[i] = clamp(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
                }

                // calculate anatomical command torques
                command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
                command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
                // command_torques[0] = 0;
                // command_torques[1] = 0;
                for (int i = 0; i < meii.N_qs_; ++i) {
                    rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
                    // rps_command_torques[i] = 0;
                }

                std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);
                

                // set anatomical command torques
                meii.set_anatomical_joint_torques(command_torques);
                

                break;
            }

            // write to MelShares
            ms_pos.write_data(aj_positions);
            ms_vel.write_data(aj_velocities);
            ms_trq.write_data(command_torques);
            ms_ref.write_data(ref);

            // update all DAQ output channels
            q8.write_all();

            // write to robot data log
            robot_log_row[0] = timer.get_elapsed_time().as_seconds();
            for (std::size_t i = 0; i < meii.N_rj_; ++i) {
                robot_log_row[3 * i + 1] = meii[i].get_position();
                robot_log_row[3 * i + 2] = meii[i].get_velocity();
                robot_log_row[3 * i + 3] = meii[i].get_torque_command();
            }
            robot_log.push_back(robot_log_row);

            // check for stop key
            int key_press = -1;
            key_press = get_key_nb();
            if (key_press == 13) {
                stop = true;
            }

            // wait for remainder of sample period
            timer.wait();

        }
        meii.disable();
        q8.disable();
    }
    
    disable_realtime();

    print("Do you want to save the robot data log? (Y/N)");
    int key_pressed = 0;
    while (key_pressed != 'y' && key_pressed != 'n'){
        key_pressed = get_key();
    }

    if (key_pressed == 'y'){
        csv_write_row(filepath, header);
        csv_append_rows("example_meii_robot_data_log.csv", robot_log);
    } 

    while (get_key_nb() != 0);

    return 0;
}


