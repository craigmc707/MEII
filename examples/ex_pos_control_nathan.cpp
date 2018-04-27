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

	// sample period for DAQ
	Time Ts = milliseconds(1); 

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
		std::vector<std::string> log_header = { "Time [s]", "ref 1 [rad/s]", "ref 2 [rad/s]",  "ref 3 [rad/s]", "ref 4 [rad/s]", "ref 5 [rad/s]"};
		robot_log.set_header(log_header);
		robot_log.set_record_format(DataFormat::Default, 12);
		bool save_data = false;

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
		std::vector<double> ref = { -35 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 00 * DEG2RAD, 0.09 };

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
		/*q8.watchdog.start();*/
		state_clock.restart();
		while (!stop) {

			// begin switch state
			switch (state) {
			case 0: // backdrive

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
							}
							keypress_refract_clock.restart();
							print("Press 'L' for a linear trajectory, or 'D' for a dmp trajectory.");
						}
					}
				}

				// depending on DOF, create the start and end points of trajectories
				neutral_point = neutral_point_set[dof];
				extreme_points = extreme_points_set[dof];
				dmp_duration = dmp_durations[dof];

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

					// check for exit key
					if (Keyboard::is_key_pressed(Key::Escape)) {
						stop = true;
						save_data = false;
					}
				}

				// Make sure trajectory is valid
				if (!dmp.trajectory().validate()) {
					LOG(Warning) << "DMP trajectory invalid.";
					return 0;
				}

				// check for wait period to end
				if (traj_selected) {

					dof_selected = false;
					traj_selected = false;

					// generate new trajectories
					if (traj_type == "linear")
					{
						waypoints[0] = neutral_point.set_time(Time::Zero);
						waypoints[1] = extreme_points[current_extreme_idx].set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}
					else if (traj_type == "dmp")
					{
						dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
					}

					ref_traj_clock.restart();
					state = 1;
					LOG(Info) << "Going to Extreme position";
					state_clock.restart();
				}
				break;


			case 1: // go to extreme position


				// move along reference trajectory for either linear or dmp
				if (traj_type=="linear")
				{

					ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());
				}
				else if (traj_type == "dmp")
				{
					ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());
				}

				if (ref_traj_clock.get_elapsed_time() >= dmp.trajectory().back().when()) {
					
					// set the ref to the last point of the trajectory
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
					LOG(Info) << "Waiting at extreme position";
					state_clock.restart();
				}
				break;

			case 2: // wait at extreme position

				// update reference from trajectory

				if (state_clock.get_elapsed_time() > wait_at_extreme_time) {

					// generate new trajectories
					if (traj_type == "linear")
					{
						waypoints[0] = extreme_points[current_extreme_idx].set_time(Time::Zero);
						waypoints[1] = neutral_point.set_time(dmp_duration);
						ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
					}

					else if (traj_type == "dmp")
					{
						dmp.set_endpoints(extreme_points[current_extreme_idx].set_time(Time::Zero), neutral_point.set_time(dmp_duration));
					}

					// change which extreme position will be visited next
					if (current_extreme_idx == 0)
					{
						current_extreme_idx++;
					}
					else
					{
						current_extreme_idx--;
					}
					
					state = 3;
					
					LOG(Info) << "Going to neutral position.";
					state_clock.restart();
					ref_traj_clock.restart();
				}
				break;

			case 3: // go to neutral position

				// move along reference trajectory for either linear or dmp
				if (traj_type == "linear")
				{
					ref = ref_traj.at_time(ref_traj_clock.get_elapsed_time());
				}
				else if (traj_type == "dmp")
				{
					ref = dmp.trajectory().at_time(ref_traj_clock.get_elapsed_time());
				}

				if (ref_traj_clock.get_elapsed_time() >= dmp.trajectory().back().when()) {

					// set the ref to the last point of the trajectory
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

					LOG(Info) << "Waiting at neutral position.";

					state = 4;
					state_clock.restart();
				}
				break;

			case 4: // wait at neutral position

				// check if wait time has passed
				if (state_clock.get_elapsed_time() > wait_at_extreme_time) {

					// if only the first extreme has been visited
					if (current_extreme_idx == 1) {

						// generate new trajectories
						if (traj_type == "linear")
						{
							waypoints[0] = neutral_point.set_time(Time::Zero);
							waypoints[1] = extreme_points[current_extreme_idx].set_time(dmp_duration);
							ref_traj.set_waypoints(5, waypoints, Trajectory::Interp::Linear, traj_max_diff);
						}
						else if (traj_type == "dmp")
						{
							dmp.set_endpoints(neutral_point.set_time(Time::Zero), extreme_points[current_extreme_idx].set_time(dmp_duration));
						}

						state = 1;
						LOG(Info) << "Going to extreme position.";
					}

					// if both extrema have been visited, prompt for next trajectory
					else {
						state = 0;

						LOG(Info) << "Waiting at neutral position for user input.";

						print("Press number key for selecting single DoF trajectory.");
						print("1 = Elbow Flexion/Extension");
						print("2 = Wrist Pronation/Supination");
						print("3 = Wrist Flexion/Extension");
						print("4 = Wrist Radial/Ulnar Deviation");
						print("Press 'Escape' to exit the program.");
					}

					state_clock.restart();
					ref_traj_clock.restart();
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
				robot_log_row[i+1] = ref[i];
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