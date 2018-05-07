#include <MEL/Daq/Quanser/Q8Usb.hpp>
#include <MEII/MahiExoII/MahiExoII.hpp>
#include <MEL/Utility/System.hpp>
#include <MEL/Communications/Windows/MelShare.hpp>
#include <MEL/Utility/Options.hpp>
#include <MEL/Core/Timer.hpp>
#include <MEL/Math/Functions.hpp>
#include <MEL/Logging/Log.hpp>
#include <MEL/Logging/DataLogger.hpp>
#include <MEL/Utility/Console.hpp>
#include <MEL/Utility/Windows/Keyboard.hpp>
#include <MEII/Control/Trajectory.hpp>
#include <MEII/Control/DynamicMotionPrimitive.hpp>
#include <MEII/Control/MinimumJerk.hpp>
#include <MEL/Math/Integrator.hpp>
#include <MEII/OpenSim/osim_utility.hpp>
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
	Options options("ex_pos_control_nathan.exe", "Nathan's Position Control Demo");
	options.add_options()
		("c,calibrate", "Calibrates the MAHI Exo-II")
		("s,single", "MAHI Exo-II follows a single-DoF trajectory generated by a DMP")
		("m,multi", "MAHI Exo-II follows a multi-DoF trajectory generated by a DMP")
		("i,int", "Enter an interger", value<int>())
		("h,help", "Prints this help message");

	auto result = options.parse(argc, argv);

	// if -h, print the help option
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

	// construct Q8 USB and configure    
	Q8Usb q8;
	q8.digital_output.set_enable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_disable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_expire_values(std::vector<Logic>(8, High));
	if (!q8.identify(7)) {
		LOG(Error) << "Incorrect DAQ";
		return 0;
	}
	Time Ts = milliseconds(1); // sample period for DAQ

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

	// calibrate - manually zero the encoders (right arm supinated)
	if (result.count("calibrate") > 0) {
		meii.calibrate(stop);
		LOG(Info) << "MAHI Exo-II encoders calibrated.";
		return 0;
	}

	// make MelShares
	MelShare ms_pos("ms_pos");
	MelShare ms_vel("ms_vel");
	MelShare ms_trq("ms_trq");
	MelShare ms_ref("ms_ref");

	// create ranges
	std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
	{ -90 * DEG2RAD, 90 * DEG2RAD },
	{ -15 * DEG2RAD, 15 * DEG2RAD },
	{ -15 * DEG2RAD, 15 * DEG2RAD },
	{ 0.08, 0.115 } };

	// construct timer in hybrid mode to avoid using 100% CPU
	Timer timer(Ts, Timer::Hybrid);

	// construct clock for regulating keypress
	Clock keypress_refract_clock;
	Time keypress_refract_time = seconds(0.5);

	// define DOFs
	enum DoF {
		ElbowFE, // ElbowFE = 0 by default
		WristPS, // WristPS = 1
		WristFE, // WristFE = 2
		WristRU, // WristRU = 3
		LastDoF
	};
	std::vector<std::string> dof_str = { "ElbowFE", "WristPS", "WristFE", "WristRU" };

	// construct robot data log
	DataLogger robot_log(WriterType::Buffered, false);
	std::vector<double> robot_log_row(6);
	std::vector<std::string> log_header = { "Time [s]", "ref 1 [rad/s]", "ref 2 [rad/s]",  "ref 3 [rad/s]", "ref 4 [rad/s]", "ref 5 [rad/s]" };
	robot_log.set_header(log_header);
	robot_log.set_record_format(DataFormat::Default, 12);
	bool save_data = false;

	// trajectory following
	if (result.count("single") > 0) {
		LOG(Info) << "MAHI Exo-II Trajectory Following.";

		// setup trajectories
		std::size_t num_full_cycles = 2;
		std::size_t current_cycle = 0;
		std::vector<WayPoint> neutral_point_set = {
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 })
		};
		std::vector<std::vector<WayPoint>> extreme_points_set = {
			{ WayPoint(Time::Zero,{ -05 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
			{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
			{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
			{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 0.09 }) }
		};
		std::vector<Time> dmp_durations = { seconds(5.0), seconds(5.0), seconds(5.0), seconds(5.0) };
		std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 25 * mel::DEG2RAD, 25 * mel::DEG2RAD, 0.1 };
		Time time_to_start = seconds(3.0);
		Time dmp_Ts = milliseconds(50);

		// default dmp traj
		DynamicMotionPrimitive dmp(dmp_Ts, neutral_point_set[0], extreme_points_set[0][0].set_time(dmp_durations[0]));
		dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);

		// Initializing variables for linear travel
		WayPoint initial_waypoint;
		std::vector<WayPoint> waypoints(2);
		Trajectory ref_traj;
		WayPoint current_wp;
		WayPoint next_wp;

		//default mj traj
		MinimumJerk mj(dmp_Ts, neutral_point_set[0], extreme_points_set[0][0].set_time(dmp_durations[0]));
		mj.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);

		// Initializing variables for dmp
		DoF dof = ElbowFE; // default
		WayPoint neutral_point;
		std::vector<WayPoint> extreme_points;
		Time dmp_duration;
		bool dof_selected = false;
		bool traj_selected = false;

		std::string traj_type;
		std::size_t current_extreme_idx = 0;

		// construct clocks for waiting and trajectory
		Clock state_clock;
		Clock ref_traj_clock;

		// set up state machine
		std::size_t state = 0;
		Time backdrive_time = seconds(1);
		Time wait_at_neutral_time = seconds(1);
		Time wait_at_extreme_time = seconds(1);

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

		// prompt user for input
		print("Press 'Escape' to exit the program.");
		print("Press 'Enter' to exit the program and save data.");

		print("Press number key for selecting single DoF trajectory.");
		print("1 = Elbow Flexion/Extension");
		print("2 = Wrist Pronation/Supination");
		print("3 = Wrist Flexion/Extension");
		print("4 = Wrist Radial/Ulnar Deviation");
		print("Press 'Escape' to exit the program.");

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

			// begin switch state
			switch (state) {
			case 0: // backdrive

				// update ref, though not being used
				ref = meii.get_anatomical_joint_positions();

				for (size_t i = 0; i < meii.N_aj_; i++){
					command_torques[i] = 0.0;
				}

				// command zero torque
				meii.set_joint_torques(command_torques);

				int number_keypress;
				//bool save_data = true;
				if (!dof_selected) {

					// check for number keypress
					number_keypress = Keyboard::is_any_num_key_pressed();
					if (number_keypress >= 0) {
						if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
							if (number_keypress > 0 && number_keypress <= 4) {
								dof = (DoF)(number_keypress - 1);
								dof_selected = true;
								LOG(Info) << dof_str[dof] << " selected.";

								// depending on DOF, create the start and end points of trajectories
								neutral_point = neutral_point_set[dof];
								extreme_points = extreme_points_set[dof];
								dmp_duration = dmp_durations[dof];
								print("Press 'L' for a linear trajectory, 'D' for a dmp trajectory, or 'M' for a minimum jerk trajectory.");
							}
							keypress_refract_clock.restart();
						}
					}
				}

				// prompt user for input to select which trajectory
				if (dof_selected && !traj_selected) {

					// press D for dmp trajectory
					if (Keyboard::is_key_pressed(Key::D)) {
						traj_selected = true;
						traj_type = "dmp";
					}

					// press L for dmp trajectory
					if (Keyboard::is_key_pressed(Key::L)) {
						traj_selected = true;
						traj_type = "linear";
					}

					// press L for dmp trajectory
					if (Keyboard::is_key_pressed(Key::M)) {
						traj_selected = true;
						traj_type = "min_jerk";
					}

					// check for exit key
					if (Keyboard::is_key_pressed(Key::Escape)) {
						stop = true;
						save_data = false;
					}
				}

				// check for wait period to end
				if (traj_selected) {

					dof_selected = false;
					traj_selected = false;

					ref_traj_clock.restart();
					state = 1;
					LOG(Info) << "Going to Extreme position";
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
					LOG(Info) << "Going to neutral position.";
					meii.set_rps_control_mode(2); // platform height NON-backdrivable                   
					
					// generate new trajectories
					if (traj_type == "linear"){
						waypoints[0] = WayPoint(Time::Zero, meii.get_anatomical_joint_positions());
						waypoints[1] = neutral_point.set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}
					else if (traj_type == "dmp"){
						dmp.set_endpoints(WayPoint(Time::Zero, meii.get_anatomical_joint_positions()), neutral_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						ref_traj = dmp.trajectory();
					}
					else if (traj_type == "min_jerk"){
						mj.set_endpoints(WayPoint(Time::Zero, meii.get_anatomical_joint_positions()), neutral_point.set_time(dmp_duration));
						if (!mj.trajectory().validate()) {
							LOG(Warning) << "MJ trajectory invalid.";
							stop = true;
						}
						ref_traj = mj.trajectory();
					}

					ref_traj_clock.restart();
					state_clock.restart();
				}
				break;

			case 2: // go to neutral position

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
					return 0; //HERE IS WHERE IT ENDS FOR NOW
					state = 3;
					ref = ref_traj.back().get_pos();
					LOG(Info) << "Waiting at neutral position.";
					state_clock.restart();
				}

				break;

			case 3: // wait at neutral position

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
				if (state_clock.get_elapsed_time() > wait_at_neutral_time) {
					if (current_extreme_idx >= extreme_points.size()) {
						current_cycle++;
						current_extreme_idx = 0;
					}
					if (current_cycle < num_full_cycles) {
						state = 4;
						LOG(Info) << "Going to extreme position.";

						// generate new trajectories
						if (traj_type == "linear"){
							waypoints[0] = neutral_point.set_time(Time::Zero);
							waypoints[1] = extreme_points[current_extreme_idx].set_time(dmp_duration);
							ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
						}
						else if (traj_type == "dmp"){
							dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
							if (!dmp.trajectory().validate()) {
								LOG(Warning) << "DMP trajectory invalid.";
								stop = true;
							}
							ref_traj = dmp.trajectory();
						}
						else if (traj_type == "min_jerk"){
							mj.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
							if (!mj.trajectory().validate()) {
								LOG(Warning) << "MJ trajectory invalid.";
								stop = true;
							}
							ref_traj = mj.trajectory();
						}

						state_clock.restart();
						ref_traj_clock.restart();
					}
					else {
						state = 0;
						LOG(Info) << "Trajectory finished.";
						state_clock.restart();
						ref_traj_clock.restart();
					}

				}

				break;
			
			case 4: // go to extreme position

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
					state = 5;
					ref = ref_traj.back().get_pos();
					LOG(Info) << "Waiting at extreme position.";
					state_clock.restart();
				}

				break;

			case 5: // wait at extreme position

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
				if (state_clock.get_elapsed_time() > wait_at_extreme_time) {
					current_extreme_idx++;
					state = 2;
					LOG(Info) << "Going to neutral position.";

					// generate new trajectories
					if (traj_type == "linear"){
						waypoints[0] = WayPoint(Time::Zero, ref);
						waypoints[1] = neutral_point.set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}
					else if (traj_type == "dmp"){
						dmp.set_endpoints(WayPoint(Time::Zero, ref), neutral_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						ref_traj = dmp.trajectory();
					}
					else if (traj_type == "min_jerk"){
						mj.set_endpoints(WayPoint(Time::Zero, ref), neutral_point.set_time(dmp_duration));
						if (!mj.trajectory().validate()) {
							LOG(Warning) << "MJ trajectory invalid.";
							stop = true;
						}
						ref_traj = mj.trajectory();
					}

					dmp.set_endpoints(WayPoint(Time::Zero, ref), neutral_point.set_time(dmp_duration));
					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}

				break;
			}


			// write ref to MelShares
			ms_ref.write_data(ref);

			// check for save key
			if (Keyboard::is_key_pressed(Key::Enter)) {
				stop = true;
				save_data = true;
			}

			// check for exit key
			if (Keyboard::is_key_pressed(Key::Escape)) {
				stop = true;
				save_data = false;
			}

			// store the time and ref data to log to a csv
			robot_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < 5; ++i) {
				robot_log_row[i + 1] = ref[i];
			}
			robot_log.buffer(robot_log_row);

			// wait for remainder of sample period
			timer.wait();
		}

		// save the data if the user wants
		if (save_data) {
			print("Do you want to save the robot data log? (Y/N)");
			Key key = Keyboard::wait_for_any_keys({ Key::Y, Key::N });
			if (key == Key::Y) {
				robot_log.save_data("example_meii_robot_data_log.csv", ".", false);
				robot_log.wait_for_save();
			}
		}
	}

	disable_realtime();
	return 0;
}