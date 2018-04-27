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
#include <MEII/PhriLearning/DynamicMotionPrimitive.hpp>
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
		("i,int","Enter an interger", value<int>())
		("h,help", "Prints this help message");

	auto result = options.parse(argc, argv);

	//keys.reserve(result.size());
	//auto x = result[0].as<String>();
	//print(x);

	//for (auto kv : result) {
	//	print(kv.first);
	//}

	//if (result.count("int"))
	//	print(result["int"].as<int>());

	//if (result.count("single"))
	//	print(result["single"].as<String>());

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
	/*Q8Usb q8;
	q8.digital_output.set_enable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_disable_values(std::vector<Logic>(8, High));
	q8.digital_output.set_expire_values(std::vector<Logic>(8, High));
	if (!q8.identify(7)) {
		LOG(Error) << "Incorrect DAQ";
		return 0;
	}*/
	Time Ts = milliseconds(1); // sample period for DAQ

	// create MahiExoII and bind Q8 channels to it
	/*std::vector<Amplifier> amplifiers;
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
	MahiExoII meii(config);*/

	// calibrate - manually zero the encoders (right arm supinated)
	//if (result.count("calibrate") > 0) {
	//	meii.calibrate(stop);
	//	LOG(Info) << "MAHI Exo-II encoders calibrated.";
	//	return 0;
	//}

	// make MelShares
	MelShare ms_pos("ms_pos");
	MelShare ms_vel("ms_vel");
	MelShare ms_trq("ms_trq");
	MelShare ms_ref("ms_ref");
	MelShare ms_emg("ms_emg");

	// trajectory following
	if (result.count("single") > 0) {
		LOG(Info) << "MAHI Exo-II Trajectory Following.";

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


		// construct robot data log
		DataLogger robot_log(WriterType::Buffered, false);
		std::vector<double> robot_log_row(6);
		std::vector<std::string> log_header = { "Time [s]", "ref 1 [rad/s]", "ref 2 [rad/s]",  "ref 3 [rad/s]", "ref 4 [rad/s]", "ref 5 [rad/s]"};
		robot_log.set_header(log_header);
		robot_log.set_record_format(DataFormat::Default, 12);
		bool save_data = false;

		// prompt user for input to select which DoF
		print("Press number key for selecting single DoF trajectory.");
		print("1 = Elbow Flexion/Extension");
		print("2 = Wrist Pronation/Supination");
		print("3 = Wrist Flexion/Extension");
		print("4 = Wrist Radial/Ulnar Deviation");
		print("Press 'Escape' to exit the program.");
		int number_keypress;
		bool dof_selected = false;
		//bool save_data = true;
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

		// prompt user for input to select which trajectory
		print("Press 'L' for a linear trajectory, or 'D' for a dmp trajectory.");
		std::string traj_type;
		bool traj_selected = false;
		while (!traj_selected && !stop) {

			if (Keyboard::is_key_pressed(Key::D)) {
				traj_selected = true;
				traj_type = "dmp";
			}

			// check for exit key
			if (Keyboard::is_key_pressed(Key::L)) {
				traj_selected = true;
				traj_type = "linear";
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
		std::vector<Time> dmp_durations = { seconds(5.0), seconds(5.0), seconds(5.0), seconds(5.0) };
		WayPoint neutral_point = neutral_point_set[dof];
		std::vector<WayPoint> extreme_points = extreme_points_set[dof];
		Time dmp_duration = dmp_durations[dof];
		std::vector<double> traj_max_diff = { 50 * mel::DEG2RAD, 50 * mel::DEG2RAD, 25 * mel::DEG2RAD, 25 * mel::DEG2RAD, 0.1 };
		Time time_to_start = seconds(3.0);
		Time dmp_Ts = milliseconds(50);
		DynamicMotionPrimitive dmp(dmp_Ts, neutral_point, extreme_points[0].set_time(dmp_duration));
		dmp.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);

		// for linear travel
		WayPoint initial_waypoint;
		std::vector<WayPoint> waypoints(2);
		Trajectory ref_traj;

		if (!dmp.trajectory().validate()) {
			LOG(Warning) << "DMP trajectory invalid.";
			return 0;
		}
		WayPoint current_wp;
		WayPoint next_wp;
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
		/*std::vector<double> rj_positions(meii.N_rj_);
		std::vector<double> rj_velocities(meii.N_rj_);
		std::vector<double> aj_positions(meii.N_aj_);
		std::vector<double> aj_velocities(meii.N_aj_);
		std::vector<double> command_torques(meii.N_aj_, 0.0);
		std::vector<double> rps_command_torques(meii.N_qs_, 0.0);*/
		std::vector<double> ref = { -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 };

		// enable DAQ and exo
		/*q8.enable();
		meii.enable();*/

		// initialize controller
		/*meii.set_rps_control_mode(0);*/

		// prompt user for input
		print("Press 'Escape' to exit the program.");
		print("Press 'Enter' to exit the program and save data.");

		// start loop
		LOG(Info) << "Robot Backdrivable.";
		/*q8.watchdog.start();*/
		state_clock.restart();
		while (!stop) {

			// begin switch state
			switch (state) {
			case 0: // backdrive
				

				// check for wait period to end
				if (state_clock.get_elapsed_time() >= backdrive_time) {

					// linear
					if (traj_type == "linear")
					{
						waypoints[0] = neutral_point.set_time(Time::Zero);
						waypoints[1] = extreme_points[current_extreme_idx].set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}

					// dmp
					if (traj_type == "dmp")
					{
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
					}

					ref_traj_clock.restart();
					state = 1;
					LOG(Info) << "Initializing RPS Mechanism.";
					state_clock.restart();
				}
				break;


			case 1: // go to extreme position
				if (traj_type=="linear")
				{
					ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());
				}
				else if (traj_type == "dmp")
				{
					ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());
				}

				if (ref_traj_clock.get_elapsed_time() >= dmp.trajectory().back().when()) {

					if (traj_type == "linear")
					{
						ref = ref_traj.back().get_pos();
					}
					else if (traj_type == "dmp")
					{
						ref = dmp.trajectory().back().get_pos();
					}
					

					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}

					state = 2;
					state_clock.restart();
				}
				break;

			case 2: // wait at extreme position

				// update reference from trajectory

				if (state_clock.get_elapsed_time() > wait_at_extreme_time) {

					// linear
					if (traj_type == "linear")
					{
						waypoints[0] = extreme_points[current_extreme_idx].set_time(Time::Zero);
						waypoints[1] = neutral_point.set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}

					// dmp
					if (traj_type == "dmp")
					{
						dmp.set_endpoints(extreme_points[current_extreme_idx].set_time(Time::Zero), neutral_point.set_time(dmp_duration));
					}

					current_extreme_idx++;
					state = 3;
					
					LOG(Info) << "Waiting at neutral position.";
					state_clock.restart();
					ref_traj_clock.restart();
				}
				break;

			case 3: // go to neutral position

				if (traj_type == "linear")
				{
					ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());
				}
				else if (traj_type == "dmp")
				{
					ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());
				}

				if (ref_traj_clock.get_elapsed_time() >= dmp.trajectory().back().when()) {

					if (traj_type == "linear")
					{
						ref = ref_traj.back().get_pos();
					}
					else if (traj_type == "dmp")
					{
						ref = dmp.trajectory().back().get_pos();
					}

					if (!dmp.trajectory().validate()) {
						LOG(Warning) << "DMP trajectory invalid.";
						stop = true;
					}

					state = 4;
					state_clock.restart();
				}
				break;

			case 4: // wait at neutral position

				// check if wait time has passed
				if (state_clock.get_elapsed_time() > wait_at_extreme_time) {

					if (current_extreme_idx == 1) {
						
						// linear
						if (traj_type == "linear")
						{
							waypoints[0] = neutral_point.set_time(Time::Zero);
							waypoints[1] = extreme_points[current_extreme_idx].set_time(dmp_duration);
							ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
						}

						// dmp
						else if (traj_type == "dmp")
						{
							dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
						}

						state = 1;
					}
					else {
						current_extreme_idx--;
						state = 5;
					}
					LOG(Info) << "Waiting at neutral position.";
					state_clock.restart();
					ref_traj_clock.restart();
				}
				break;
		}


			// write to MelShares
			/*ms_pos.write_data(aj_positions);
			ms_vel.write_data(aj_velocities);
			ms_trq.write_data(command_torques);
			ms_ref.write_data(ref);*/

			// write to MelShares
			ms_ref.write_data(ref);

			// write to data log
			/*meii_mot_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_mot_log_row[i + 1] = meii[i].get_position();
			}
			meii_mot_log.push_back_row(meii_mot_log_row);
			meii_sto_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < meii.N_rj_; ++i) {
				meii_sto_log_row[i + 1] = meii[i].get_torque();
			}
			meii_sto_log.push_back_row(meii_sto_log_row);*/

			// update all DAQ output channels
			/*q8.update_output();*/

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
			/*if (!q8.watchdog.kick() || meii.any_limit_exceeded())
				stop = true;*/

			

			robot_log_row[0] = timer.get_elapsed_time().as_seconds();
			for (std::size_t i = 0; i < 5; ++i) {
				robot_log_row[i+1] = ref[i];
			}

			robot_log.buffer(robot_log_row);

			// wait for remainder of sample period
			timer.wait();
		}

		/*meii.disable();
		q8.disable();*/

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