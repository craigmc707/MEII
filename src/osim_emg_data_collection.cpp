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
#include <MEII/Control/DynamicMotionPrimitive.hpp>
#include <MEL/Math/Integrator.hpp>
#include <MEII/OpenSim/osim_utility.hpp>
#include <MEII/Utility/logging_util.hpp>
#include <MEII/EMG/MesArray.hpp>
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
		("c,calibrate", "Calibrates the MAHI Exo-II")
		("s,single", "MAHI Exo-II follows a single-DoF trajectory generated by a DMP")
		("m,multi", "MAHI Exo-II follows a multi-DoF trajectory generated by a DMP")
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

	// construct Q8 USB and configure    
	Q8Usb q8(QOptions(), true, true, emg_channel_numbers); // specify all EMG channels
	q8.digital_output.set_enable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_disable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_expire_values(std::vector<Logic>(8, High));
	if (!q8.identify(7)) {
		LOG(Error) << "Incorrect DAQ";
		return 0;
	}
	emg_channel_numbers = q8.analog_input.get_channel_numbers();
	std::size_t emg_channel_count = q8.analog_input.get_channel_count();
	Time Ts = milliseconds(1); // sample period for DAQ

	// construct Myoelectric Signal (MES) Array
	MesArray mes(q8.analog_input.get_channels(emg_channel_numbers), 300);

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
	MelShare ms_emg("ms_emg");

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

	enum DoF {
		ElbowFE, // ElbowFE = 0 by default
		WristPS, // WristPS = 1
		WristFE, // WristFE = 2
		WristRU, // WristRU = 3
		LastDoF
	};
	std::vector<std::string> dof_str = { "ElbowFE", "WristPS", "WristFE", "WristRU" };


	// construct data logs
	MeiiOsimMotTable meii_mot_log;
	std::vector<double> meii_mot_log_row(meii_mot_log.col_count());
	MeiiOsimStoTable meii_sto_log;
	std::vector<double> meii_sto_log_row(meii_sto_log.col_count());
	MeiiTable meii_std_log;
	std::vector<double> meii_std_log_row(meii_std_log.col_count());
	EmgTable emg_std_log("EmgTable", emg_channel_numbers, false, true, true, false);
	std::vector<double> emg_std_log_row(emg_std_log.col_count());
	bool save_data = true;

	// construct clocks for waiting and trajectory
	Clock state_clock;
	Clock ref_traj_clock;

	// set up state machine
	std::size_t state = 0;
	Time backdrive_time = seconds(3);
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

	// single-dof trajectory following
	if (result.count("single") > 0) {
		LOG(Info) << "MAHI Exo-II Single-DoF Trajectory Following.";

		// path for output data file
		std::string output_path = "C:" + get_path_slash() + "Git" + get_path_slash() + "MEII_OpenSim" + get_path_slash() + "RecordedData";

		// prompt user for input to select which DoF
		print("Press number key for selecting a single-DoF trajectory.");
		print("1 = Elbow Flexion/Extension");
		print("2 = Wrist Pronation/Supination");
		print("3 = Wrist Flexion/Extension");
		print("4 = Wrist Radial/Ulnar Deviation");
		print("Press 'Escape' to exit the program.");
		int number_keypress;
		bool dof_selected = false;
		DoF dof = ElbowFE; // default
		while (!dof_selected && !stop) {

			// check for number keypress
			number_keypress = Keyboard::is_any_num_key_pressed();
			if (number_keypress >= 0) {
				if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
					if (number_keypress > 0 && number_keypress <= 4) {
						dof = (DoF)(number_keypress - 1);
						dof_selected = true;
						LOG(Info) << dof_str[dof] << " selected.";
					}
					keypress_refract_clock.restart();
				}
			}

			// check for exit key
			if (Keyboard::is_key_pressed(Key::Escape)) {
				stop = true;
				save_data = false;
			}

			// wait for remainder of sample period
			timer.wait();
		}

		// setup trajectory
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
		WayPoint final_point(Time::Zero, { -15 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 0.12 });
		std::vector<Time> dmp_durations = { seconds(5.0), seconds(5.0), seconds(5.0), seconds(5.0) };
		WayPoint neutral_point = neutral_point_set[dof];
		std::vector<WayPoint> extreme_points = extreme_points_set[dof];
		Time dmp_duration = dmp_durations[dof];
		std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 25 * mel::DEG2RAD, 25 * mel::DEG2RAD, 0.1 };
		Time time_to_start = seconds(3.0);
		Time dmp_Ts = milliseconds(50);
		DynamicMotionPrimitive dmp(dmp_Ts, neutral_point, extreme_points[0].set_time(dmp_duration));
		dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);
		if (!dmp.trajectory().validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}
		WayPoint current_wp;
		WayPoint next_wp;
		std::size_t current_extreme_idx = 0;
		bool on_single_dof_trajectory = false;

		// enable DAQ and exo
		q8.enable();
		meii.enable();

		// initialize controller
		meii.set_rps_control_mode(0);

		// prompt user for input
		print("Press 'Escape' to exit the program.");
		print("Press 'Enter' to exit the program and save data.");

		// start loop
		LOG(Info) << "Robot Backdrivable.";
		q8.watchdog.start();
		state_clock.restart();
		while (!stop) {

			// update all DAQ input channels
			q8.update_input();

			// update MahiExoII kinematics
			meii.update_kinematics();

			// update EMG signal processing
			mes.update_and_buffer();

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
					LOG(Info) << "Going to neutral position.";
					meii.set_rps_control_mode(2); // platform height NON-backdrivable                   
					dmp.set_endpoints(WayPoint(Time::Zero, meii.get_anatomical_joint_positions()), neutral_point.set_time(dmp_duration));
					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}
				break;

			case 2: // go to neutral position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					state = 3;
					on_single_dof_trajectory = true;
					ref = dmp.trajectory().back().get_pos();
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
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}
					else {
						state = 6;
						LOG(Info) << "Going to final position.";
						on_single_dof_trajectory = false;
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), final_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}

				}

				break;

			case 4: // go to extreme position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					state = 5;
					ref = dmp.trajectory().back().get_pos();
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
					dmp.set_endpoints(WayPoint(Time::Zero, ref), neutral_point.set_time(dmp_duration));
					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}

				break;

			case 6: // go to final position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					stop = true;
					LOG(Info) << "Finished.";
				}

				break;

			} // end switch state

			// write to MelShares
			ms_pos.write_data(aj_positions);
			ms_vel.write_data(aj_velocities);
			ms_trq.write_data(command_torques);
			ms_ref.write_data(ref);

			if (on_single_dof_trajectory) {
				// write to osim motion data log
				meii_mot_log_row[0] = timer.get_elapsed_time().as_seconds();
				for (std::size_t i = 0; i < meii.N_rj_; ++i) {
					meii_mot_log_row[i + 1] = meii[i].get_position();
				}
				meii_mot_log.push_back_row(meii_mot_log_row);

				// write to osim controls data log
				meii_sto_log_row[0] = timer.get_elapsed_time().as_seconds();
				for (std::size_t i = 0; i < meii.N_rj_; ++i) {
					meii_sto_log_row[i + 1] = meii[i].get_torque();
				}
				meii_sto_log.push_back_row(meii_sto_log_row);
			}

			// write to MEII standard data log
			meii_std_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1] = meii[i].get_position();
			}
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1 + meii.N_rj_] = meii[i].get_velocity();
			}
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1 + 2 * meii.N_rj_] = meii[i].get_torque();
			}
			meii_std_log.push_back_row(meii_std_log_row);

			// write to EMG standard data log
			emg_std_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1] = mes.get_demean()[i];
			}
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1 + emg_channel_count] = mes.get_envelope()[i];
			}
			emg_std_log.push_back_row(emg_std_log_row);

			// update all DAQ output channels
			q8.update_output();

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

			// kick watchdog
			if (!q8.watchdog.kick() || meii.any_limit_exceeded())
				stop = true;

			// wait for remainder of sample period
			timer.wait();

		}
		meii.disable();
		q8.disable();

		if (save_data) {
			write_meii_to_osim_mot(meii_mot_log, dof_str[dof] + "_" + "dmp" + "_" + "meii_coordinate_positions", ".", false);
			write_meii_to_osim_sto(meii_sto_log, dof_str[dof] + "_" + "dmp" + "_" + "meii_coordinate_actuator_controls", ".", false);
			DataLogger::write_to_csv(meii_std_log, dof_str[dof] + "_" + "dmp" + "_" + "meii_std_log", ".", false);
			DataLogger::write_to_csv(emg_std_log, dof_str[dof] + "_" + "dmp" + "_" + "emg_std_log", ".", false);
		}

	} // single-dof trajectory following



	// multi-dof trajectory following
	if (result.count("multi") > 0) {
		LOG(Info) << "MAHI Exo-II Multi-DoF Trajectory Following.";

		// path for output data file
		std::string output_path = "C:" + get_path_slash() + "Git" + get_path_slash() + "MEII_OpenSim" + get_path_slash() + "RecordedData";

		// prompt user for input to select which DoF
		print("Press number key for selecting a multi-DoF trajectory.");
		print("1 = Elbow Flexion/Extension and Wrist Pronation/Supination");
		print("2 = Wrist Flexion/Extension and Wrist Radial/Ulnar Deviation");
		print("Press 'Escape' to exit the program.");
		int number_keypress;
		bool dof_selected = false;
		std::size_t multi_dof_index = 0; // default
		DoF first_dof = ElbowFE; // default
		DoF second_dof = WristPS; // default
		while (!dof_selected && !stop) {

			// check for number keypress
			number_keypress = Keyboard::is_any_num_key_pressed();
			if (number_keypress >= 0) {
				if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
					if (number_keypress > 0 && number_keypress <= 2) {
						multi_dof_index = number_keypress - 1;
						first_dof = (DoF)(number_keypress * 2 - 2);
						second_dof = (DoF)(number_keypress * 2 - 1);
						dof_selected = true;
						LOG(Info) << dof_str[first_dof] << " and " << dof_str[second_dof] << " selected.";
					}
					keypress_refract_clock.restart();
				}
			}

			// check for exit key
			if (Keyboard::is_key_pressed(Key::Escape)) {
				stop = true;
				save_data = false;
			}

			// wait for remainder of sample period
			timer.wait();
		}

		// setup trajectory
		std::size_t num_full_cycles = 2;
		std::size_t current_cycle = 0;
		std::vector<WayPoint> neutral_point_set = {
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
			WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 })
		};
		std::vector<std::vector<WayPoint>> extreme_points_set = {
			{ WayPoint(Time::Zero,{ -05 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -05 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
			{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD,-15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD,-15 * DEG2RAD, 0.09 }) }
		};
		WayPoint final_point(Time::Zero, { -15 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 0.12 });
		std::vector<Time> dmp_durations = { seconds(5.0), seconds(5.0) };
		WayPoint neutral_point = neutral_point_set[multi_dof_index];
		std::vector<WayPoint> extreme_points = extreme_points_set[multi_dof_index];
		Time dmp_duration = dmp_durations[multi_dof_index];
		std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 25 * mel::DEG2RAD, 25 * mel::DEG2RAD, 0.1 };
		Time time_to_start = seconds(3.0);
		Time dmp_Ts = milliseconds(50);
		DynamicMotionPrimitive dmp(dmp_Ts, neutral_point, extreme_points[0].set_time(dmp_duration));
		dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);
		if (!dmp.trajectory().validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}
		WayPoint current_wp;
		WayPoint next_wp;
		std::size_t current_extreme_idx = 0;
		bool on_multi_dof_trajectory = false;

		// enable DAQ and exo
		q8.enable();
		meii.enable();

		// initialize controller
		meii.set_rps_control_mode(0);

		// prompt user for input
		print("Press 'Escape' to exit the program.");
		print("Press 'Enter' to exit the program and save data.");

		// start loop
		LOG(Info) << "Robot Backdrivable.";
		q8.watchdog.start();
		state_clock.restart();
		while (!stop) {

			// update all DAQ input channels
			q8.update_input();

			// update MahiExoII kinematics
			meii.update_kinematics();

			// update EMG signal processing
			mes.update_and_buffer();

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
					LOG(Info) << "Going to neutral position.";
					meii.set_rps_control_mode(2); // platform height NON-backdrivable                   
					dmp.set_endpoints(WayPoint(Time::Zero, meii.get_anatomical_joint_positions()), neutral_point.set_time(dmp_duration));
					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}
				break;

			case 2: // go to neutral position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					state = 3;
					on_multi_dof_trajectory = true;
					ref = dmp.trajectory().back().get_pos();
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
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}
					else {
						state = 6;
						LOG(Info) << "Going to final position.";
						on_multi_dof_trajectory = false;
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), final_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}

				}

				break;

			case 4: // go to extreme position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					state = 5;
					ref = dmp.trajectory().back().get_pos();
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
					dmp.set_endpoints(WayPoint(Time::Zero, ref), neutral_point.set_time(dmp_duration));
					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}
					ref_traj_clock.restart();
					state_clock.restart();
				}

				break;

			case 6: // go to final position

					// update reference from trajectory
				ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());

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
				if (ref_traj_clock.get_elapsed_time() > dmp.trajectory().back().when()) {
					stop = true;
					LOG(Info) << "Finished.";
				}

				break;

			} // end switch state

			  // write to MelShares
			ms_pos.write_data(aj_positions);
			ms_vel.write_data(aj_velocities);
			ms_trq.write_data(command_torques);
			ms_ref.write_data(ref);

			if (on_multi_dof_trajectory) {
				// write to osim motion data log
				meii_mot_log_row[0] = timer.get_elapsed_time().as_seconds();
				for (std::size_t i = 0; i < meii.N_rj_; ++i) {
					meii_mot_log_row[i + 1] = meii[i].get_position();
				}
				meii_mot_log.push_back_row(meii_mot_log_row);

				// write to osim controls data log
				meii_sto_log_row[0] = timer.get_elapsed_time().as_seconds();
				for (std::size_t i = 0; i < meii.N_rj_; ++i) {
					meii_sto_log_row[i + 1] = meii[i].get_torque();
				}
				meii_sto_log.push_back_row(meii_sto_log_row);
			}

			// write to MEII standard data log
			meii_std_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1] = meii[i].get_position();
			}
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1 + meii.N_rj_] = meii[i].get_velocity();
			}
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_std_log_row[i + 1 + 2 * meii.N_rj_] = meii[i].get_torque();
			}
			meii_std_log.push_back_row(meii_std_log_row);

			// write to EMG standard data log
			emg_std_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1] = mes.get_demean()[i];
			}
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1 + emg_channel_count] = mes.get_envelope()[i];
			}
			emg_std_log.push_back_row(emg_std_log_row);

			// update all DAQ output channels
			q8.update_output();

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

			// kick watchdog
			if (!q8.watchdog.kick() || meii.any_limit_exceeded())
				stop = true;

			// wait for remainder of sample period
			timer.wait();

		}
		meii.disable();
		q8.disable();

		if (save_data) {
			write_meii_to_osim_mot(meii_mot_log, dof_str[first_dof] + "_and_" + dof_str[second_dof] + "_" + "dmp" + "_" + "meii_coordinate_positions", ".", false);
			write_meii_to_osim_sto(meii_sto_log, dof_str[first_dof] + "_and_" + dof_str[second_dof] + "_" + "dmp" + "_" + "meii_coordinate_actuator_controls", ".", false);
			DataLogger::write_to_csv(meii_std_log, dof_str[first_dof] + "_and_" + dof_str[second_dof] + "_" + "dmp" + "_" + "meii_std_log", ".", false);
			DataLogger::write_to_csv(emg_std_log, dof_str[first_dof] + "_and_" + dof_str[second_dof] + "_" + "dmp" + "_" + "emg_std_log", ".", false);
		}

	} // multi-dof trajectory following


	mel::disable_realtime();
	return 0;
}


