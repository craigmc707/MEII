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
#include <MEL/Math/Integrator.hpp>
#include <MEII/OpenSim/osim_utility.hpp>
#include <MEII/Utility/logging_util.hpp>
#include <MEII/EMG/MesArray.hpp>
#include <MEL/Devices/Myo/MyoBand.hpp>
#include <MEII/Classification/EmgActiveEnsClassifier.hpp>
#include <MEII/Classification/EmgDirClassifier.hpp>
#include <MEII/EMG/EmgDataCapture.hpp>
#include <vector>

using namespace mel;
using namespace meii;


// create global stop variable CTRL-C handler function
ctrl_bool stop(false);
bool handler(CtrlEvent event) {
	stop = true;
	return true;
}

bool is_single_dof(std::size_t dof_index) {
	return dof_index < 4;
}


int main(int argc, char *argv[]) {

	// make options
	Options options("ex_myo_armband_ctrl.exe", "Demo of the Myo Armband controlling the MAHI Exo-II");
	options.add_options()
		("c,calibrate", "Calibrates the MAHI Exo-II")
		("k,keyboard", "Runs the demo using only keyboard controls")
		("u,unity", "Runs the demo with Unity visual interface")
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

	// set emg channel numbers
	std::vector<uint32> emg_channel_numbers = { 0, 1, 2, 3, 4, 5, 6, 7 };
	std::size_t emg_channel_count = emg_channel_numbers.size();

	// construct and enable myo
	MyoBand myo("my_myo");
	myo.enable();

	// construct Myoelectric Signal (MES) Array
	MesArray mes(myo.get_channels(emg_channel_numbers), 300);	

	// initialize EMG classification and data capture variables
	std::size_t num_classes = 2;
	std::size_t active_state = 0;
	std::size_t selected_dir = 0;
	std::size_t pred_class = 0;
	std::size_t pred_dir = 0;
	std::size_t true_class = 0;
	bool save_active_detector = false;
	bool save_dir_classifier = false;
	bool RMS = true;
	bool MAV = false;
	bool WL = false;
	bool ZC = false;
	bool SSC = false;
	bool AR1 = false;
	bool AR2 = false;
	bool AR3 = false;
	bool AR4 = false;
	Time mes_rest_capture_period = seconds(1);
	Time mes_active_capture_period = seconds(1);
	Time mes_active_period = seconds(0.2);
	Time mes_dir_capture_period = seconds(0.2);
	Time active_training_refract_time = mes_active_capture_period;
	Time dir_training_refract_time = seconds(1);
	Time dir_pred_refract_time = seconds(2);
	std::vector<Key> active_keys = { Key::Num1, Key::Num2, Key::Num3, Key::Num4 };

	// set data capture variables
	std::size_t mes_rest_capture_window_size = (std::size_t)((unsigned)(mes_rest_capture_period.as_seconds() / Ts.as_seconds()));
	std::size_t mes_active_capture_window_size = (std::size_t)((unsigned)(mes_active_capture_period.as_seconds() / Ts.as_seconds()));
	std::size_t mes_dir_capture_window_size = (std::size_t)((unsigned)(mes_dir_capture_period.as_seconds() / Ts.as_seconds()));
	mes.resize_buffer(std::max(mes_rest_capture_window_size, mes_active_capture_window_size));
	active_training_refract_time = seconds(std::max((double)((signed)mes.get_buffer_capacity()) * Ts.as_seconds(), active_training_refract_time.as_seconds()));
	std::size_t mes_active_window_size = (std::size_t)((unsigned)(mes_active_period.as_seconds() / Ts.as_seconds()));
		
	// construct classifiers	
	EmgActiveEnsClassifier active_detector(emg_channel_count, Ts);
	EmgDirClassifier dir_classifier(num_classes, emg_channel_count, Ts, RMS, MAV, WL, ZC, SSC, AR1, AR2, AR3, AR4);

	// construct clocks for regulating classification tasks
	Clock training_refract_clock;
	Clock pred_refract_clock;

	// make MelShares
	MelShare ms_pos("ms_pos");
	MelShare ms_vel("ms_vel");
	MelShare ms_trq("ms_trq");
	MelShare ms_ref("ms_ref");
	MelShare ms_emg("ms_emg");
	MelShare ms_pred("ms_pred");		

	// construct enums for selecting dof and experiment conditions
	enum DoF {
		ElbowFE, // ElbowFE = 0 by default
		WristPS, // WristPS = 1
		WristFE, // WristFE = 2
		WristRU, // WristRU = 3
		LastDoF
	};
	std::vector<std::string> dof_str = { "ElbowFE", "WristPS", "WristFE", "WristRU" };

	enum Condition {
		Calibration, // Calibration = 0 by default
		Training, // Training = 1
		BlindTesting, // BlindTesting = 2
		FullTesting, // FullTesting = 3
		LastCondition
	};
	std::vector<std::string> cond_str = { "Calibration", "Training", "BlindTesting", "FullTesting" };

	// create robot anatomical joint space ranges
	std::vector<std::vector<double>> setpoint_rad_ranges = { { -90 * DEG2RAD, 0 * DEG2RAD },
	{ -90 * DEG2RAD, 90 * DEG2RAD },
	{ -15 * DEG2RAD, 15 * DEG2RAD },
	{ -15 * DEG2RAD, 15 * DEG2RAD },
	{ 0.08, 0.115 } };

	// initialize trajectory
	std::vector<WayPoint> neutral_point_set = {
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }),
		WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 })
	};
	std::vector<std::vector<WayPoint>> extreme_points_set = {
		{ WayPoint(Time::Zero,{ -05 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
		{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
		{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
		{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 0.09 }) },
		{ WayPoint(Time::Zero,{ -05 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -05 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD, 30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -65 * DEG2RAD,-30 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 }) },
		{ WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD, 15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD, 15 * DEG2RAD,-15 * DEG2RAD, 0.09 }), WayPoint(Time::Zero,{ -35 * DEG2RAD, 00 * DEG2RAD,-15 * DEG2RAD,-15 * DEG2RAD, 0.09 }) }
	};
	WayPoint final_point(Time::Zero, { -15 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 00 * DEG2RAD , 0.12 });
	std::vector<Time> dmp_durations = { seconds(3.0), seconds(3.0), seconds(2.0), seconds(2.0), seconds(3.0), seconds(2.0) };
	std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 45 * mel::DEG2RAD, 45 * mel::DEG2RAD, 0.1 };
	Time time_to_start = seconds(2.0);
	Time dmp_Ts = milliseconds(50);

	// construct data logs
	MeiiTable meii_std_log;
	std::vector<double> meii_std_log_row(meii_std_log.col_count());
	EmgTable emg_std_log("EmgTable", emg_channel_numbers, true, true, true, true);
	std::vector<double> emg_std_log_row(emg_std_log.col_count());
	Table testing_results_log;
	std::vector<double> testing_results_log_row;
	bool save_data = true;
	bool save_testing_results = false;

	// file management
	std::string file_prefix;

	// construct timer in hybrid mode to avoid using 100% CPU
	Timer timer(Ts, Timer::Hybrid);

	// construct clock for regulating keypress
	Clock keypress_refract_clock;
	Time keypress_refract_time = seconds(0.5);
	

	// construct clocks for waiting and trajectory
	Clock state_clock;
	Clock ref_traj_clock;

	// set up state machine
	std::size_t state = 0;
	Time backdrive_time = seconds(1);
	Time wait_at_neutral_time = seconds(0.2);
	Time wait_at_extreme_time = seconds(0.2);
	bool full_testing_first_cycle = true;

	// create data containers
	std::vector<double> rj_positions(meii.N_rj_);
	std::vector<double> rj_velocities(meii.N_rj_);
	std::vector<double> aj_positions(meii.N_aj_);
	std::vector<double> aj_velocities(meii.N_aj_);
	std::vector<double> command_torques(meii.N_aj_, 0.0);
	std::vector<double> rps_command_torques(meii.N_qs_, 0.0);
	std::vector<double> ref(meii.N_aj_, 0.0);

	// keyboard interface
	if (result.count("keyboard") > 0) {
		LOG(Info) << "Myo Armband control of MAHI Exo-II with keyboard interface.";

		// path for output data files
		std::string output_path = ".";

		// prompt user for input to select which DoF
		print("Press number key for selecting a single-DoF or multi-DoF trajectory.");
		print("1 = Elbow Flexion/Extension");
		print("2 = Wrist Pronation/Supination");
		print("3 = Wrist Flexion/Extension");
		print("4 = Wrist Radial/Ulnar Deviation");
		print("5 = Elbow Flexion/Extension and Wrist Pronation/Supination");
		print("6 = Wrist Flexion/Extension and Wrist Radial/Ulnar Deviation");
		print("Press 'Escape' to exit the program.");
		int number_keypress;
		bool dof_selected = false;
		std::size_t dof_index = 0; // default
		DoF first_dof = ElbowFE; // default
		DoF second_dof = WristPS; // default
		while (!dof_selected && !stop) {

			// check for number keypress
			number_keypress = Keyboard::is_any_num_key_pressed();
			if (number_keypress >= 0) {
				if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {		
					if (number_keypress > 0 && number_keypress <= 6) {
						dof_index = number_keypress - 1;
						dof_selected = true;
						if (is_single_dof(dof_index)) {
							first_dof = (DoF)dof_index;
							num_classes = 2;							
							LOG(Info) << dof_str[first_dof] << " selected.";
							file_prefix = dof_str[first_dof];
						}
						else {
							first_dof = (DoF)(dof_index * 2 - 8);
							second_dof = (DoF)(dof_index * 2 - 7);
							num_classes = 4;
							LOG(Info) << dof_str[first_dof] << " and " << dof_str[second_dof] << " selected.";
							file_prefix = dof_str[first_dof] + "_and_" + dof_str[second_dof];
						}
						
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

		// set classifier paramters based on selected dof
		active_detector.resize(num_classes);
		dir_classifier.set_class_count(num_classes);

		// set trajectory parameters based on selected dof				
		WayPoint neutral_point = neutral_point_set[dof_index];
		std::vector<WayPoint> extreme_points = extreme_points_set[dof_index];
		Time dmp_duration = dmp_durations[dof_index];				
		DynamicMotionPrimitive dmp(dmp_Ts, neutral_point, extreme_points[0].set_time(dmp_duration));
		dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);
		if (!dmp.trajectory().validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}		

		// prompt user for input to select which condition
		print("Press number key for selecting experimental condition.");
		print("1 = Calibration of active/rest classifier.");
		print("2 = Training of directional classifer.");
		print("3 = Testing of directional classifier without robot motion.");
		print("4 = Testing of directional classifier with robot motion");
		print("Press 'Escape' to exit the program.");
		bool condition_selected = false;
		//std::size_t condition_index = 0; // default
		Condition cond = Calibration; // default
		while (!condition_selected && !stop) {

			// check for number keypress
			number_keypress = Keyboard::is_any_num_key_pressed();
			if (number_keypress >= 0) {
				if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
					if (number_keypress > 0 && number_keypress <= 4) {
						cond = (Condition)(number_keypress - 1);
						LOG(Info) << cond_str[cond] << " selected.";
						condition_selected = true;
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

		// load classifiers based on selected condition
		if (cond != Calibration) {
			if (!active_detector.load(file_prefix + "_" + "emg_active_detector", output_path)) {
				stop = true;
				save_data = false;
				LOG(Warning) << "Active detector could not be loaded.";
			}
		}
		if (cond != Calibration && cond != Training) {
			if (!dir_classifier.load(file_prefix + "_" + "emg_directional_classifier", output_path)) {
				stop = true;
				save_data = false;
				LOG(Warning) << "Directional classifier could not be loaded.";
			}
		}

		// set logging parameters based on selected dof and condition
		if (cond == BlindTesting) {
			testing_results_log.rename("BlindTestingResults");
		}
		else if (cond == FullTesting) {
			testing_results_log.rename("FullTestingResults");
		}
		if (cond == BlindTesting || cond == FullTesting) {
			testing_results_log.push_back_col("time");
			for (std::size_t i = 0; i < dir_classifier.get_feature_dim(); ++i) {
				testing_results_log.push_back_col("phi_" + stringify(i));
			}
			for (std::size_t i = 0; i < num_classes; ++i) {
				testing_results_log.push_back_col("y_" + stringify(i));
			}
			for (std::size_t i = 0; i < num_classes; ++i) {
				testing_results_log.push_back_col("p_" + stringify(i));
			}
			testing_results_log.push_back_col("true_label");
			testing_results_log.push_back_col("pred_label");
			testing_results_log_row.resize(testing_results_log.col_count());
		}

		
		// enable DAQ and exo
		q8.enable();
		meii.enable();

		// initialize controller
		meii.set_rps_control_mode(0);

		// prompt user for input
		print("Press 'Escape' to exit the program without saving data.");

		// start loop
		LOG(Info) << "Robot Backdrivable.";
		q8.watchdog.start();
		state_clock.restart();
		while (!stop) {

			// update all DAQ input channels
			q8.update_input();
			myo.update();

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
					if (cond == Calibration) {
						state = 7;
						LOG(Info) << "Calibration of active/rest classifier.";
						print("Press 'A + 0' to add 'rest' state training data to all classifiers.");
						print("Press 'C + 0' to clear 'rest' state training data from all classifiers.");
						print("Press 'A + target #' to add 'active' state training data for one classifier.");
						print("Press 'C + target #' to clear 'active' state training data for one classifier.");
						print("Number of 'active' state classifiers is:");
						print(num_classes);
						print("Press 'T' to train classifier and begin real-time classification.");
						print("Press 'Enter' to finish and save active/rest classifier.");
						print("Press 'Escape' to exit.");
					}
					else if (cond == Training) {
						state = 8;
						LOG(Info) << "Training of direcitonal classifier.";
						print("Press target number key to enable triggered data capture for that target.");
						print("Number of possible targets is:");
						print(num_classes);
						print("Press 'T' to train direction classifier and begin real-time classification.");
						print("Press 'Enter' to finish and save directional classifier.");
						print("Press 'Escape' to exit.");
					}
					else if (cond == BlindTesting) {
						state = 9;
						LOG(Info) << "Blind testing of direcitonal classifier.";
						print("Press target number key to enable triggered predictions for that target.");
						print("Number of possible targets is:");
						print(num_classes);
						print("Press 'Enter' to finish and save testing results.");
						print("Press 'Escape' to exit.");
					}
					else if (cond == FullTesting) {
						state = 9;
						if (full_testing_first_cycle) {
							LOG(Info) << "Blind testing of direcitonal classifier with robot motion.";
							print("Press target number key to enable triggered predictions for that target.");
							print("Number of possible targets is:");
							print(num_classes);
							print("Press 'Enter' to finish and save testing results.");
							print("Press 'Escape' to exit.");
							full_testing_first_cycle = false;
						}
						else {
							LOG(Info) << "Waiting for prediction.";
							dir_classifier.clear_buffers();
						}
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

			case 7: // calibration

				// predict state
				if (active_detector.update(mes.get_tkeo_envelope())) {
					active_state = active_detector.get_class();
				}

				// write prediction to melshare
				ms_pred.write_data({ (double)active_state });

				// clear rest data
				if (Keyboard::are_all_keys_pressed({ Key::C, Key::Num0 })) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						bool cleared_successfully = true;
						for (std::size_t k = 0; k < num_classes; ++k) {
							if (!active_detector.clear_training_data(k, 0)) {
								cleared_successfully = false;
							}
						}
						if (cleared_successfully) {
							LOG(Info) << "Cleared rest data.";
						}
						keypress_refract_clock.restart();
					}
				}

				// capture rest data
				if (Keyboard::are_all_keys_pressed({ Key::A, Key::Num0 })) {
					if (mes.is_buffer_full()) {
						if (training_refract_clock.get_elapsed_time() > active_training_refract_time) {
							bool added_successfully = true;
							for (std::size_t k = 0; k < num_classes; ++k) {
								if (!active_detector.add_training_data(k, 0, mes.get_tkeo_env_buffer_data(mes_rest_capture_window_size))) {
									added_successfully = false;
								}
							}
							if (added_successfully) {
								LOG(Info) << "Added rest data.";
							}
							training_refract_clock.restart();
						}
					}
				}

				// clear active data
				for (std::size_t k = 0; k < num_classes; ++k) {
					if (Keyboard::are_all_keys_pressed({ Key::C, active_keys[k] })) {
						if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
							if (active_detector.clear_training_data(k, 1))
								LOG(Info) << "Cleared active data for target " + stringify(k + 1) + ".";
							keypress_refract_clock.restart();
						}
					}
				}

				// capture active data
				for (std::size_t k = 0; k < num_classes; ++k) {
					if (Keyboard::are_all_keys_pressed({ Key::A, active_keys[k] })) {
						if (mes.is_buffer_full()) {
							if (training_refract_clock.get_elapsed_time() > active_training_refract_time) {
								if (active_detector.add_training_data(k, 1, find_sum_max_window(mes.get_tkeo_env_buffer_data(mes_active_capture_window_size), mes_active_window_size))) {
									LOG(Info) << "Added active data for target " + stringify(k + 1) + ".";
								}
								training_refract_clock.restart();
							}
						}
					}
				}

				// train the active/rest classifiers
				if (Keyboard::is_key_pressed(Key::T)) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						if (active_detector.train())
							LOG(Info) << "Trained new active/rest classifier based on given data.";
						keypress_refract_clock.restart();
					}
				}

				// finish calibration and save the computed classifier
				if (Keyboard::is_key_pressed(Key::Enter)) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						state = 6;
						save_active_detector = true;
						training_refract_clock.restart();
						keypress_refract_clock.restart();					
						LOG(Info) << "Going to final position.";
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), final_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}
				}

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

				break;

			case 8: // training of directional classifier

				// predict state
				if (active_detector.update(mes.get_tkeo_envelope())) {
					active_state = active_detector.get_class();
					if (dir_classifier.update(mes.get_demean())) {
						pred_class = dir_classifier.get_class();
						if (active_state == 1) {
							if (pred_refract_clock.get_elapsed_time() > dir_pred_refract_time) {													
								pred_dir = pred_class + 1;
								pred_refract_clock.restart();
							}
						}
						else {
							pred_dir = 0;
						}
					}
				}

				// write prediction to melshare
				ms_pred.write_data({ (double)active_state, (double)(pred_class + 1) });

				// clear training data
				for (std::size_t k = 0; k < num_classes; ++k) {
					if (Keyboard::are_all_keys_pressed({ Key::C, active_keys[k] })) {
						if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
							if (dir_classifier.clear_training_data(k)) {
								LOG(Info) << "Cleared training data for target " + stringify(k + 1) + ".";
							}
							keypress_refract_clock.restart();
						}
					}
				}

				// add training data
				for (std::size_t k = 0; k < num_classes; ++k) {
					if (selected_dir == k + 1) {
						if (mes.is_buffer_full()) {
							if (active_state == 1) {
								if (training_refract_clock.get_elapsed_time() > dir_training_refract_time) {
									if (dir_classifier.add_training_data(k, mes.get_dm_buffer_data(mes_dir_capture_window_size))) {
										LOG(Info) << "Added training data for target " + stringify(k + 1) + ".";
									}
									training_refract_clock.restart();
								}
							}
						}
					}
				}

				// train the direction classifier
				if (Keyboard::is_key_pressed(Key::T)) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						if (dir_classifier.train()) {
							LOG(Info) << "Trained new directional classifier based on given data.";
							selected_dir = 0;
						}						
						keypress_refract_clock.restart();
					}
				}

				// update the selected target and enable active detection
				if (!Keyboard::is_key_pressed(Key::A) && !Keyboard::is_key_pressed(Key::C)) {
					number_keypress = Keyboard::is_any_num_key_pressed();
					if (number_keypress >= 0) {
						if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
							if (number_keypress == 0) {
								selected_dir = number_keypress;
								print("No target currently selected.");
							}
							if (number_keypress > 0 && number_keypress <= num_classes) {
								selected_dir = number_keypress;
								print("Current target is " + stringify(selected_dir));
							}							
							keypress_refract_clock.restart();
						}
					}
				}

				// finish training and save the computed directional classifier
				if (Keyboard::is_key_pressed(Key::Enter)) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						state = 6;
						save_dir_classifier = true;
						training_refract_clock.restart();
						keypress_refract_clock.restart();
						LOG(Info) << "Going to final position.";
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), final_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						state_clock.restart();
						ref_traj_clock.restart();
					}
				}

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

				break;


			case 9: // testing of directional classifier

				// predict state
				if (active_detector.update(mes.get_tkeo_envelope())) {
					active_state = active_detector.get_class();
					if (dir_classifier.update(mes.get_demean())) {
						pred_class = dir_classifier.get_class();					
						if (pred_refract_clock.get_elapsed_time() > dir_pred_refract_time) {
							if (active_state == 1) {
								if (selected_dir > 0) {
									true_class = selected_dir - 1;
									pred_dir = pred_class + 1;
									LOG(Info) << "Logging directional classifier prediction for class label " << true_class;
									testing_results_log_row[0] = timer.get_elapsed_time_ideal().as_seconds();
									for (std::size_t i = 0; i < dir_classifier.get_feature_dim(); ++i) {
										testing_results_log_row[i + 1] = dir_classifier.get_features()[i];
									}
									for (std::size_t i = 0; i < dir_classifier.get_class_count(); ++i) {
										testing_results_log_row[i + 1 + dir_classifier.get_feature_dim()] = dir_classifier.get_model_output()[i];
									}
									for (std::size_t i = 0; i < dir_classifier.get_class_count(); ++i) {
										testing_results_log_row[i + 1 + dir_classifier.get_feature_dim() + dir_classifier.get_class_count()] = dir_classifier.get_class_posteriors()[i];
									}
									testing_results_log_row[1 + dir_classifier.get_feature_dim() + 2 * dir_classifier.get_class_count()] = (double)true_class;
									testing_results_log_row[1 + dir_classifier.get_feature_dim() + 2 * dir_classifier.get_class_count() + 1] = (double)dir_classifier.get_class();
									testing_results_log.push_back_row(testing_results_log_row);
									if (cond == FullTesting) {
										state = 4;
										selected_dir = 0;
										LOG(Info) << "Going to extreme position.";
										dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[pred_class].set_time(dmp_duration));
										if (!dmp.trajectory().validate()) {
											LOG(Warning) << "DMP trajectory invalid.";
											stop = true;
										}
										state_clock.restart();
										ref_traj_clock.restart();
									}
									pred_refract_clock.restart();
								}
							}
							else {
								pred_dir = 0;
							}
						}						
					}
				}

				// write prediction to melshare
				ms_pred.write_data({ (double)pred_dir });


				// update the selected target and enable active detection
				if (!Keyboard::is_key_pressed(Key::A) && !Keyboard::is_key_pressed(Key::C)) {
					number_keypress = Keyboard::is_any_num_key_pressed();
					if (number_keypress >= 0) {
						if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
							if (number_keypress == 0) {
								selected_dir = number_keypress;
								print("No target currently selected.");
							}
							if (number_keypress > 0 && number_keypress <= num_classes) {
								selected_dir = number_keypress;
								print("Current target is " + stringify(selected_dir));
							}
							keypress_refract_clock.restart();
						}
					}
				}

				// finish testing and save the results
				if (Keyboard::is_key_pressed(Key::Enter)) {
					if (keypress_refract_clock.get_elapsed_time() > keypress_refract_time) {
						state = 6;
						save_testing_results = true;
						LOG(Info) << "Going to final position.";
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), final_point.set_time(dmp_duration));
						if (!dmp.trajectory().validate()) {
							LOG(Warning) << "DMP trajectory invalid.";
							stop = true;
						}
						keypress_refract_clock.restart();
						state_clock.restart();
						ref_traj_clock.restart();
					}
				}

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

				break;

			} // end switch state

			// write to MelShares
			ms_pos.write_data(aj_positions);
			ms_vel.write_data(aj_velocities);
			ms_trq.write_data(command_torques);
			ms_ref.write_data(ref);
			ms_emg.write_data(mes.get_tkeo_envelope());
			

			// write to MEII standard data log
			meii_std_log_row[0] = timer.get_elapsed_time_ideal().as_seconds();
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
			emg_std_log_row[0] = timer.get_elapsed_time_ideal().as_seconds();
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1] = mes.get_raw()[i];
			}
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1 + emg_channel_count] = mes.get_demean()[i];
			}
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1 + 2 * emg_channel_count] = mes.get_envelope()[i];
			}
			for (std::size_t i = 0; i < emg_channel_count; ++i) {
				emg_std_log_row[i + 1 + 3 * emg_channel_count] = mes.get_tkeo_envelope()[i];
			}
			emg_std_log.push_back_row(emg_std_log_row);

			// update all DAQ output channels
			q8.update_output();

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
			DataLogger::write_to_csv(meii_std_log, file_prefix + "_" + "myo_armband" + "_" + "meii_std_log", output_path, false);
			DataLogger::write_to_csv(emg_std_log, file_prefix + "_" + "myo_armband" + "_" + "emg_std_log", output_path, false);
		}

		if (save_active_detector) {
			active_detector.save(file_prefix + "_" + "emg_active_detector", output_path, false);
			LOG(Info) << "Active/rest classifier saved.";
		}

		if (save_dir_classifier) {
			dir_classifier.save(file_prefix + "_" + "emg_directional_classifier", output_path, false);
			LOG(Info) << "Directional classifier saved.";
		}

		if (save_testing_results) {
			if (cond == BlindTesting) {
				DataLogger::write_to_csv(testing_results_log, file_prefix + "_" + "myo_armband_blind_results_log", output_path, false);
				LOG(Info) << "Blind testing results log saved.";
			}
			else if (cond == FullTesting) {
				DataLogger::write_to_csv(testing_results_log, file_prefix + "_" + "myo_armband_full_results_log", output_path, false);
				LOG(Info) << "Full testing results log saved.";
			}
		}


	} // keyboard interface



	// unity interface
	if (result.count("unity") > 0) {

	} // unity interface

	mel::disable_realtime();
	return 0;
}


