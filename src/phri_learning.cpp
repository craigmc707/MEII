#include <MEL/Daq/Quanser/Q8Usb.hpp>
#include <MEII/MahiExoII/MahiExoII.hpp>
#include <MEL/Utility/System.hpp>
#include <MEL/Communications/MelShare.hpp>
#include <MEL/Utility/Options.hpp>
#include <MEL/Core/Timer.hpp>
#include <MEL/Math/Functions.hpp>
#include <MEL/Logging/Log.hpp>
#include <MEL/Logging/DataLogger.hpp>
#include <MEL/Utility/Console.hpp>
#include <MEL/Utility/Windows/Keyboard.hpp>
#include <MEII/Control/Trajectory.hpp>
#include <MEII/PhriLearning/PhriFeatures.hpp>
#include <MEII/Control/DynamicMotionPrimitive.hpp>
#include <MEL/Math/Integrator.hpp>
#include <MEII/EMG/MesArray.hpp>
#include <MEL/Devices/Myo/MyoBand.hpp>
#include <MEII/EMG/EmgDataCapture.hpp>
#include <MEII/EmgRealTimeControl/EmgRealTimeControl.hpp>
#include <MEII/EMG/EmgDirectMapping.hpp>
#include <vector>

using namespace mel;
using namespace meii;


// create global stop variable CTRL-C handler function
ctrl_bool stop(false);
bool handler(CtrlEvent event) {
    stop = true;
    return true;
}

int main(int argc, char *argv[]) {

    // make options
    Options options("phri_learning.exe", "Physical Human-Robot Interaction Learning Experiment");
    options.add_options()
        ("z,zero",  "Zeros the MAHI Exo-II encoders.")
        ("v,velocity", "MAHI Exo-II is directly velocity controlled by EMG.")
		("d,deform", "MAHI Exo-II follows a trajectory that is directly deformed by EMG.")
		("l,learning", "MAHI Exo-II follows a DMP-generated trajectory continuously updated by learning from user effort")
        ("h,help", "Prints this help message");

    auto result = options.parse(argc, argv);

    if (result.count("help") > 0) {
        print(options.help());
        return 0;
    }

    // enable Windows realtime
    enable_realtime();

    // initialize logger
    init_logger();

    // register ctrl-c handler
    register_ctrl_handler(handler);   

	// set emg channel numbers
	std::vector<uint32> emg_channel_numbers = { 0, 1, 2, 3, 4, 5, 6, 7 };
	std::size_t emg_channel_count = emg_channel_numbers.size();

    // make Q8 USB and configure
	Time Ts = milliseconds(1);
    Q8Usb q8;
    q8.digital_output.set_enable_values(std::vector<Logic>(8, High));
    q8.digital_output.set_disable_values(std::vector<Logic>(8, High));
    q8.digital_output.set_expire_values(std::vector<Logic>(8, High));
    if (!q8.identify(7)) {
        LOG(Error) << "Incorrect DAQ";
        return 0;
    }

    // create MahiExoII and bind Q8 channels to it
    std::vector<Amplifier> amplifiers;
    std::vector<double> amp_gains;
    for (uint32 i = 0; i < 2; ++i) {
        amplifiers.push_back(
            Amplifier("meii_amp_" + std::to_string(i),
                Low,
                q8.digital_output[i + 1],
                1.8,
                q8.analog_output[i + 1])
        );
    }
    for (uint32 i = 2; i < 5; ++i) {
        amplifiers.push_back(
            Amplifier("meii_amp_" + std::to_string(i),
                Low,
                q8.digital_output[i + 1],
                0.184,
                q8.analog_output[i + 1])
        );
    }
    MeiiConfiguration config(q8, q8.watchdog, q8.encoder[{1, 2, 3, 4, 5}], q8.velocity[{1, 2, 3, 4, 5}], amplifiers);
    MahiExoII meii(config);   

    // zero - manually zero the encoders (right arm supinated)
    if (result.count("zero") > 0) {       
        meii.calibrate(stop);
        LOG(Info) << "MAHI Exo-II encoders zeroed.";
        return 0;
    }
	
	// initialize robot control variables
	std::vector<double> velocity_control_scalars = {
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD,
		20 * mel::DEG2RAD };
	std::vector<double> joint_torque_scalars = {
		1,
		1,
		1,
		1,
		1,
		1,
		1,
		1 };

	// construct and enable myo
	MyoBand myo("my_myo");
	myo.enable();

	// initialize data capture variables		
	Time mes_baseline_capture_period = seconds(1);
	Time mes_active_capture_period = seconds(5);

	// set data capture variables
	std::size_t mes_baseline_capture_window_size = (std::size_t)((unsigned)(mes_baseline_capture_period.as_seconds() / Ts.as_seconds()));
	std::size_t mes_active_capture_window_size = (std::size_t)((unsigned)(mes_active_capture_period.as_seconds() / Ts.as_seconds()));
	Clock baseline_capture_clock;
	Clock active_capture_clock;

	// construct Myoelectric Signal (MES) Array
	std::size_t mes_buffer_capacity = std::max(mes_baseline_capture_window_size, mes_active_capture_window_size);
	MesArray mes(myo.get_channels(emg_channel_numbers), mes_buffer_capacity);

	// construct regressor
	EmgDirectMapping mes_map(emg_channel_count, Ts);
	mes_map.set_scaling(joint_torque_scalars);
	std::vector<double> pred(emg_channel_count, 0.0);


    // make MelShares
    MelShare ms_pos("ms_pos");
    MelShare ms_vel("ms_vel");
    MelShare ms_trq("ms_trq");
    MelShare ms_ref("ms_ref");
    MelShare ms_phi("ms_phi");

    // velocity control
    if (result.count("velocity") > 0) {
        LOG(Info) << "MAHI Exo-II Trajectory Velocity Control.";

        // create ranges
        std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
        { -90 * DEG2RAD, 90 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { -15 * DEG2RAD, 15 * DEG2RAD },
        { 0.08, 0.115 } };

        // create trajectory and DMP
        std::vector<double> traj_max_diff = { 30 * mel::DEG2RAD, 10 * mel::DEG2RAD, 5 * mel::DEG2RAD, 5 * mel::DEG2RAD, 0.01 };
		Time time_to_start = seconds(3.0);
		Time dmp_duration = seconds(10.0);
        WayPoint dmp_start(Time::Zero, { -65 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0.09 });
		WayPoint dmp_goal(dmp_duration, { -5 * DEG2RAD, 30 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0.09 });
        Trajectory ref_traj;
		ref_traj.set_interp_method(Trajectory::Interp::Linear);
		ref_traj.set_max_diff(traj_max_diff);
		std::vector<double> theta = { 0.0 };
		Time dmp_Ts = milliseconds(50);
		DynamicMotionPrimitive dmp(dmp_Ts, dmp_start, dmp_goal, &feature_gradient, theta);
		Trajectory dmp_ref_traj = dmp.trajectory();
		dmp_ref_traj.set_interp_method(Trajectory::Interp::Linear);
		dmp_ref_traj.set_max_diff(traj_max_diff);
		if (!dmp_ref_traj.validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}

        // construct clocks for waiting and trajectory
		Clock state_clock;
        Clock ref_traj_clock;

        // set up state machine
        std::size_t state = 0;
        Time backdrive_time = seconds(3);
		Time wait_at_start_time = seconds(1);
		Time wait_at_goal_time = seconds(1);

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
        Timer timer(Ts, Timer::Hybrid);

        // start loop
        LOG(Info) << "Robot Backdrivable.";
        q8.watchdog.start();
		state_clock.restart();
        while (!stop) {

            // update all DAQ input channels
            q8.update_input();

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
                if (state_clock.get_elapsed_time() >= backdrive_time) {
                    meii.rps_init_par_ref_.start(meii.get_wrist_parallel_positions(), timer.get_elapsed_time());
                    state = 1;
                    LOG(Info) << "Initializing RPS Mechanism.";
					state_clock.restart();
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
					state = 2;
                    LOG(Info) << "RPS initialization complete.";
                    meii.set_rps_control_mode(2); // platform height NON-backdrivable                   
                    WayPoint current_point(seconds(0.0), aj_positions);
					ref_traj.clear();
					ref_traj.push_back(current_point);
					dmp_start.set_time(time_to_start);
					ref_traj.push_back(dmp_start);
					if (!ref_traj.validate()) {
						stop = true;
					}
                    ref_traj_clock.restart();
					state_clock.restart();
                }
                break;

			case 2: // got to initial position

				// update reference from trajectory
				ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for end of trajectory
				if (ref_traj_clock.get_elapsed_time() > ref_traj.back().when()) {
					state = 3;
					ref = ref_traj.back().get_pos();
					LOG(Info) << "Waiting at start of DMP trajectory.";
					state_clock.restart();
				}

				break;

			case 3: // wait at initial position

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for wait period to end
				if (state_clock.get_elapsed_time() > wait_at_start_time) {
					state = 4;
					LOG(Info) << "Starting DMP trajectory following.";
					state_clock.restart();
					ref_traj_clock.restart();
				}

				break;

            case 4: // DMP trajectory following

                // update reference from trajectory
                ref = dmp_ref_traj.at_time(ref_traj_clock.get_elapsed_time());

                // constrain trajectory to be within range
                for (std::size_t i = 0; i < meii.N_aj_; ++i) {
                    ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
                }

                // calculate anatomical command torques
                command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
                command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
                for (std::size_t i = 0; i < meii.N_qs_; ++i) {
                    rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
                }
                std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);
                
                // set anatomical command torques
                meii.set_anatomical_joint_torques(command_torques);

				// check for end of trajectory
				if (ref_traj_clock.get_elapsed_time() > dmp_ref_traj.back().when()) {
					state = 5;
					ref = dmp_ref_traj.back().get_pos();
					LOG(Info) << "Waiting at end of DMP trajectory.";
					state_clock.restart();
				}
                
                break;

			case 5: // wait at goal position

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for wait period to end
				if (state_clock.get_elapsed_time() > wait_at_goal_time) {
					stop = true;
					LOG(Info) << "Finished.";
				}

				break;

            }

            // write to MelShares
            ms_pos.write_data(aj_positions);
            ms_vel.write_data(aj_velocities);
            ms_trq.write_data(command_torques);
            ms_ref.write_data(ref);

            // update all DAQ output channels
            q8.update_output();

            //// write to robot data log
            //robot_log_row[0] = timer.get_elapsed_time().as_seconds();
            //for (std::size_t i = 0; i < meii.N_rj_; ++i) {
            //    robot_log_row[3 * i + 1] = meii[i].get_position();
            //    robot_log_row[3 * i + 2] = meii[i].get_velocity();
            //    robot_log_row[3 * i + 3] = meii[i].get_torque();
            //}
            //robot_log.buffer(robot_log_row);

            // check for save key
            if (Keyboard::is_key_pressed(Key::Enter)) {
                stop = true;
                //save_data = true;
            }

            // check for exit key
            if (Keyboard::is_key_pressed(Key::Escape)) {
                stop = true;
            }

            // kick watchdog
            if (!q8.watchdog.kick() || meii.any_limit_exceeded())
                stop = true;

            // wait for remainder of sample period
            timer.wait();

        }
        meii.disable();
        q8.disable();

    } // trajectory following


	// learning desired trajectory
	if (result.count("learning") > 0) {
		LOG(Info) << "MAHI Exo-II Trajectory Following with Learning.";

		// create ranges
		std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
		{ -90 * DEG2RAD, 90 * DEG2RAD },
		{ -15 * DEG2RAD, 15 * DEG2RAD },
		{ -15 * DEG2RAD, 15 * DEG2RAD },
		{ 0.08, 0.115 } };

		// create trajectory and DMP
		std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 15 * mel::DEG2RAD, 15 * mel::DEG2RAD, 0.1 };
		Time time_to_start = seconds(3.0);
		Time dmp_duration = seconds(20.0);
		WayPoint dmp_start(Time::Zero, { -65 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0, 0.09 });
		WayPoint dmp_goal(dmp_duration, { -5 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0 * DEG2RAD, 0.09 });
		Trajectory ref_traj;
		ref_traj.set_interp_method(Trajectory::Interp::Linear);
		ref_traj.set_max_diff(traj_max_diff);
		std::vector<double> theta = { 0.0 };
		std::vector<double> theta_dot = { 0.0 };
		Time dmp_Ts = milliseconds(50);
		DynamicMotionPrimitive dmp(dmp_Ts, dmp_start, dmp_goal, &feature_gradient, theta);
		Trajectory dmp_ref_traj = dmp.trajectory();
		dmp_ref_traj.set_interp_method(Trajectory::Interp::Linear);
		dmp_ref_traj.set_max_diff(traj_max_diff);
		if (!dmp_ref_traj.validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}
		WayPoint current_wp;
		WayPoint next_wp;

		// updating parameter estimate theta
		double alpha = 1.0; // update rate constant
		double s = 1.0;
		std::vector<Integrator> theta_integrator(theta.size());
		for (std::size_t i = 0; i < theta_integrator.size(); ++i) {
			theta_integrator[i].set_init(theta[i]);
		}
		Matrix Phi;
		std::vector<double> phi(1, 0.0);
		std::vector<double> u_h(meii.N_aj_, 0.0);
		std::vector<double> theta_dot_max = { 0.5 };

		// construct clocks for waiting and trajectory
		Clock state_clock;
		Clock ref_traj_clock;

		// set up state machine
		std::size_t state = 0;
		Time backdrive_time = seconds(3);
		Time wait_at_start_time = seconds(1);
		Time wait_at_goal_time = seconds(1);

		// create data containers
		std::vector<double> rj_positions(meii.N_rj_);
		std::vector<double> rj_velocities(meii.N_rj_);
		std::vector<double> aj_positions(meii.N_aj_);
		std::vector<double> aj_velocities(meii.N_aj_);
		std::vector<double> command_torques(meii.N_aj_, 0.0);
		std::vector<double> rps_command_torques(meii.N_qs_, 0.0);
		std::vector<double> ref(meii.N_aj_, 0.0);
		std::vector<double> ref_dot(meii.N_aj_, 0.0);

		// create experiment data logs
		Table results_log("Results");
		results_log.push_back_col("time");
		for (std::size_t i = 0; i < u_h.size(); ++i) {
			results_log.push_back_col("u_h_" + stringify(i));
		}
		for (std::size_t i = 0; i < phi.size(); ++i) {
			results_log.push_back_col("phi_" + stringify(i));
		}
		for (std::size_t i = 0; i < theta.size(); ++i) {
			results_log.push_back_col("theta_" + stringify(i));
		}
		for (std::size_t i = 0; i < theta_dot.size(); ++i) {
			results_log.push_back_col("theta_dot_" + stringify(i));
		}
		for (std::size_t i = 0; i < aj_positions.size(); ++i) {
			results_log.push_back_col("q_" + stringify(i));
		}
		for (std::size_t i = 0; i < aj_velocities.size(); ++i) {
			results_log.push_back_col("q_dot_" + stringify(i));
		}
		for (std::size_t i = 0; i < aj_positions.size(); ++i) {
			results_log.push_back_col("q_d_" + stringify(i));
		}
		for (std::size_t i = 0; i < aj_velocities.size(); ++i) {
			results_log.push_back_col("q_d_dot_" + stringify(i));
		}
		for (std::size_t i = 0; i < command_torques.size(); ++i) {
			results_log.push_back_col("tau_" + stringify(i));
		}
		std::vector<double> log_row(results_log.col_count());
		std::vector<double>::iterator it_log_row;

		// enable DAQ and exo
		q8.enable();
		meii.enable();

		// initialize controller
		meii.set_rps_control_mode(0);

		// construct timer in hybrid mode to avoid using 100% CPU
		Timer timer(Ts, Timer::Hybrid);

		// start loop
		LOG(Info) << "Robot Backdrivable.";
		q8.watchdog.start();
		state_clock.restart();
		while (!stop) {

			// update all DAQ input channels
			q8.update_input();

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
				if (state_clock.get_elapsed_time() >= backdrive_time) {
					meii.rps_init_par_ref_.start(meii.get_wrist_parallel_positions(), timer.get_elapsed_time());
					state = 1;
					LOG(Info) << "Initializing RPS Mechanism.";
					state_clock.restart();
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
					state = 2;
					LOG(Info) << "RPS initialization complete.";
					meii.set_rps_control_mode(2); // platform height NON-backdrivable                   
					WayPoint current_point(seconds(0.0), aj_positions);
					ref_traj.clear();
					ref_traj.push_back(current_point);
					dmp_start.set_time(time_to_start);
					ref_traj.push_back(dmp_start);
					if (!ref_traj.validate()) {
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}
				break;

			case 2: // got to initial position

				// update reference from trajectory
				ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for end of trajectory
				if (ref_traj_clock.get_elapsed_time() > ref_traj.back().when()) {
					state = 3;
					ref = ref_traj.back().get_pos();
					LOG(Info) << "Waiting at start of DMP trajectory.";
					state_clock.restart();
				}

				break;

			case 3: // wait at initial position

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for wait period to end
				if (state_clock.get_elapsed_time() > wait_at_start_time) {
					state = 4;
					LOG(Info) << "Starting DMP trajectory following.";
					state_clock.restart();
					ref_traj_clock.restart();
					theta[0] = 1.0;
					current_wp.set_time(Time::Zero);
					current_wp.set_pos(ref);
				}

				break;

			case 4: // DMP trajectory following

				//// update human effort using keyboard
				//if (Keyboard::is_key_pressed(Key::Up)) {
				//	u_h[1] += 0.01;
				//}
				//else if (Keyboard::is_key_pressed(Key::Down)) {
				//	u_h[1] -= 0.01;
				//}
				//else {
				//	if (u_h[1] != 0) {
				//		u_h[1] -= (u_h[1] / std::abs(u_h[1])) * 0.01;
				//	}
				//}

				// update the human effort using EMG direct mapping
				if(pred[4] > pred[0]) {
					u_h[1] = pred[0];
				}
				else {
					u_h[1] = -pred[4];
				}

				// update estimate of feature weights
				s = std::exp(-dmp.get_gamma() * ref_traj_clock.get_elapsed_time().as_seconds() / dmp.get_tau());
				phi = feature_extraction(Matrix(meii.get_anatomical_joint_positions())).get_col(0);
				Phi = feature_jacobian(Matrix(meii.get_anatomical_joint_positions()));
				theta_dot = (Phi * (alpha * s)) * u_h;
				
				for (std::size_t i = 0; i < theta.size(); ++i) {
					theta_dot[i] = saturate(theta_dot[i], theta_dot_max[i]);
					theta[i] = theta_integrator[i].update(theta_dot[i], ref_traj_clock.get_elapsed_time());
				}

				// log data				
				log_row[0] = ref_traj_clock.get_elapsed_time().as_seconds();
				it_log_row = std::copy(u_h.begin(), u_h.end(), log_row.begin() + 1);
				it_log_row = std::copy(phi.begin(), phi.end(), it_log_row);
				it_log_row = std::copy(theta.begin(), theta.end(), it_log_row);
				it_log_row = std::copy(theta_dot.begin(), theta_dot.end(), it_log_row);
				it_log_row = std::copy(aj_positions.begin(), aj_positions.end(), it_log_row);
				results_log.push_back_row(log_row);

				// update trajectory
				dmp.update(theta);
				if (!dmp.trajectory().validate()) {
					LOG(Warning) << "DMP Trajectory invalid.";
				}


				// check if trajectory is changing too quickly
				next_wp.set_time(ref_traj_clock.get_elapsed_time());
				next_wp.set_pos(dmp.trajectory().at_time(next_wp.when()));
				for (std::size_t j = 0; j < current_wp.get_dim(); ++j) {
					if (std::abs(next_wp[j] - current_wp[j]) / (next_wp.when().as_seconds() - current_wp.when().as_seconds()) > traj_max_diff[j]) {
						LOG(Warning) << "Trajectory changing too quickly: theta_dot = " << theta_dot;
						stop = true;
					}
				}
				current_wp = next_wp;

				// update reference from trajectory
				//ref = dmp_ref_traj.at_time(ref_traj_clock.get_elapsed_time());
				ref = next_wp.get_pos();
				

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for end of trajectory
				if (ref_traj_clock.get_elapsed_time() > dmp_ref_traj.back().when()) {
					state = 5;
					ref = dmp_ref_traj.back().get_pos();
					LOG(Info) << "Waiting at end of DMP trajectory.";
					state_clock.restart();
				}

				break;

			case 5: // wait at goal position

				// constrain trajectory to be within range
				for (std::size_t i = 0; i < meii.N_aj_; ++i) {
					ref[i] = saturate(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
				}

				// calculate anatomical command torques
				command_torques[0] = meii.anatomical_joint_pd_controllers_[0].calculate(ref[0], meii[0].get_position(), 0, meii[0].get_velocity());
				command_torques[1] = meii.anatomical_joint_pd_controllers_[1].calculate(ref[1], meii[1].get_position(), 0, meii[1].get_velocity());
				for (std::size_t i = 0; i < meii.N_qs_; ++i) {
					rps_command_torques[i] = meii.anatomical_joint_pd_controllers_[i + 2].calculate(ref[i + 2], meii.get_anatomical_joint_position(i + 2), 0, meii.get_anatomical_joint_velocity(i + 2));
				}
				std::copy(rps_command_torques.begin(), rps_command_torques.end(), command_torques.begin() + 2);

				// set anatomical command torques
				meii.set_anatomical_joint_torques(command_torques);

				// check for wait period to end
				if (state_clock.get_elapsed_time() > wait_at_goal_time) {
					stop = true;
					LOG(Info) << "Finished.";
				}

				break;

			}

			// write to MelShares
			ms_pos.write_data(aj_positions);
			ms_vel.write_data(aj_velocities);
			ms_trq.write_data(command_torques);
			ms_ref.write_data(ref);

			// update all DAQ output channels
			q8.update_output();

			//// write to robot data log
			//robot_log_row[0] = timer.get_elapsed_time().as_seconds();
			//for (std::size_t i = 0; i < meii.N_rj_; ++i) {
			//	robot_log_row[3 * i + 1] = meii[i].get_position();
			//	robot_log_row[3 * i + 2] = meii[i].get_velocity();
			//	robot_log_row[3 * i + 3] = meii[i].get_torque();
			//}
			//robot_log.buffer(robot_log_row);

			// check for save key
			if (Keyboard::is_key_pressed(Key::Enter)) {
				stop = true;
				//save_data = true;
			}

			// check for exit key
			if (Keyboard::is_key_pressed(Key::Escape)) {
				stop = true;
			}

			// kick watchdog
			if (!q8.watchdog.kick() || meii.any_limit_exceeded())
				stop = true;

			// wait for remainder of sample period
			timer.wait();

		}
		meii.disable();
		q8.disable();

		DataLogger::write_to_csv(results_log);

	} // learning

	

    //if (save_data) {
    //    print("Do you want to save the robot data log? (Y/N)");
    //    Key key = Keyboard::wait_for_any_keys({ Key::Y, Key::N });
    //    if (key == Key::Y) {
    //        robot_log.save_data("phri_robot_data_log.csv", ".", false);
    //        robot_log.wait_for_save();
    //    }
    //}

    disable_realtime();
    return 0;
}


