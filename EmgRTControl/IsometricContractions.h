
#pragma once
#include "StateMachine.h"
#include "Q8Usb.h"
#include "MahiExoIIEmg.h"
#include "MelShare.h"
#include "Clock.h"
#include "mel_util.h"
#include "mahiexoii_util.h"
#include "ExternalApp.h"


using namespace mel;

class IsometricContractionsData : public util::EventData {

public:

};

class IsometricContractions : public util::StateMachine {

public:

    //---------------------------------------------------------------------
    // CONSTRUCTOR(S) / DESTRUCTOR(S)
    //---------------------------------------------------------------------

    IsometricContractions(util::Clock& clock, core::Daq* daq, exo::MahiExoIIEmg& meii);

private:

    //-------------------------------------------------------------------------
    // STATE MACHINE SETUP
    //-------------------------------------------------------------------------

    // STATES
    enum States {
        ST_WAIT_FOR_GUI,
        ST_INIT,
        ST_BACKDRIVE,
        ST_INIT_RPS,
        ST_TO_CENTER,
        ST_HOLD_CENTER,
        ST_PRESENT_TARGET,
        ST_PROCESS_EMG,
        ST_TRAIN_CLASSIFIER,
        ST_CLASSIFY,
        ST_FINISH,
        ST_STOP,
        ST_NUM_STATES
    };

    // STATE FUNCTIONS
    void sf_wait_for_gui(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_wait_for_gui> sa_wait_for_gui;

    void sf_init(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_init> sa_init;

    void sf_backdrive(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_backdrive> sa_backdrive;

    void sf_init_rps(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_init_rps> sa_init_rps;

    void sf_to_center(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_to_center> sa_to_center;

    void sf_hold_center(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_hold_center> sa_hold_center;

    void sf_present_target(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_present_target> sa_present_target;

    void sf_process_emg(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_process_emg> sa_process_emg;

    void sf_train_classifier(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_train_classifier> sa_train_classifier;

    void sf_classify(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_classify> sa_classify;

    void sf_finish(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_finish> sa_finish;

    void sf_stop(const util::NoEventData*);
    util::StateAction<IsometricContractions, util::NoEventData, &IsometricContractions::sf_stop> sa_stop;

    // STATE MAP
    virtual const util::StateMapRow* get_state_map() {
        static const util::StateMapRow STATE_MAP[] = {
            &sa_wait_for_gui,
            &sa_init,
            &sa_backdrive,
            &sa_init_rps,
            &sa_to_center,
            &sa_hold_center,
            &sa_present_target,
            &sa_process_emg,
            &sa_train_classifier,
            &sa_classify,
            &sa_finish,
            &sa_stop,
        };
        return &STATE_MAP[0];
    }

    //-------------------------------------------------------------------------
    // EXPERIMENT SETUP & COMPONENTS
    //-------------------------------------------------------------------------

    // CONDITION
    int dof_ = 0; // 0-3 is single-dof; 4-5 is multi-dof
    int condition_ = 0; // 0 = training; 1 = blind testing; 2 = full testing
    std::vector<std::string> hand_defs = { "L","R" };
    int hand_num_ = 1; // 0 or 1 for Left or Right arm of the user
    std::string hand_def_ = hand_defs[hand_num_];

    // UNITY GAME
    util::ExternalApp game = mel::util::ExternalApp("2D_targets", "C:\\Users\\Ted\\GitHub\\MEII\\Exo Visualization\\Builds\\Exo_Vis_Build_1.exe");

    // FILE DIRECTORIES
    std::string program_directory_ = "C:\\Users\\Ted\\GitHub\\MEII\\bin\\";

    // HARDWARE CLOCK
    util::Clock clock_;

    // HARDWARE
    core::Daq* daq_;
    exo::MahiExoIIEmg meii_;

    // INPUT CLASS LABELS
    std::vector<int> class_label_sequence_;
    int current_class_label_ = 0;

    // PREDEFINED TARGETS
    const double_vec center_pos_ = { -35 * math::DEG2RAD, 0 * math::DEG2RAD, 0 * math::DEG2RAD, 0 * math::DEG2RAD,  0.09 }; // anatomical joint positions
    
    // EXPERIMENT TIMING PARAMETERS
    const double init_backdrive_time_ = 2.0; // [s] time to be in backdrive mode initially
    const double hold_center_time_ = 1.0; // time to hold at center target [s]

                                          // TASK FORCE MEASUREMENT
    int task_force_measurement_mode_ = 0; // 0 = commanded torques, dof/direction specific; 1 = commanded torques, dof specific, direction agnostic; 2 = commanded torques, dof/direction agnostic
    const std::vector<std::vector<double_vec>> force_dof_scale_ = { { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } }, // elbow f/e single-dof
                                                                    { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } }, // forearm p/s single-dof
                                                                    { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } }, // wrist f/e single-dof
                                                                    { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } }, // wrist r/u single-dof
                                                                    { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 },{ 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } }, // elbow f/e & forearm p/s multi-dof
                                                                    { { 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 },{ 1.50, 0.20, 0.15, 0.15 },{ 1.50, 0.40, 0.15, 0.15 } } }; // wrist f/e & wrist r/u multi-dof
    const std::vector<char_vec> target_dir_L_ = {
        { 1, -1 },
        { -1, 1 },
        { -1, 1 },
        { 1, -1 },
        { 1, 1, -1, -1 },
        { -1, 1, -1, 1 },
        { 1, 1, -1, -1 },
        { 1, -1, 1, -1 } };
    const std::vector<char_vec> target_dir_R_ = {
        { 1, -1 },
        { 1, -1 },
        { 1, -1 },
        { 1, -1 },
        { 1, 1, -1, -1 },
        { 1, -1, 1, -1 },
        { 1, 1, -1, -1 },
        { -1, 1, -1, 1 } };
    const double_vec gravity_offsets_ = { -0.0, 0.0, 0.0, -0.35, 0.0 }; // for all 5 anatomical dofs, due to counterweight elbow can be under 'negative gravity'
    double force_mag_goal_ = 3050.0; // 
    double force_mag_tol_ = 300.0; //
    double force_mag_dwell_time_ = 1.0; // [s]

    // FROCE MAGNITUDE CHECKING ALGORITHM VARIABLES
    double force_mag_maintained_ = 0.0;
    double force_mag_time_now_ = 0.0;
    double force_mag_time_last_ = 0.0;

    // UNITY INPUT/OUTPUT
    int SCENE_NUM_ = 0; // capitalized because it is the external data provided from GUI and should be handled carefully!
    int viz_target_num_ = 0;
    comm::MelShare scene_num_share_ = comm::MelShare("scene_num");
    comm::MelShare viz_target_num_share_ = comm::MelShare("target");
    comm::MelShare force_mag_share_ = comm::MelShare("force_mag");
    comm::MelShare hand_select_ = comm::MelShare("hand");

    // EMG SENSING AND FEATURE EXTRACTION PARAMETERS
    static const int num_emg_channels_ = 8;
    static const int num_features_ = 9;
    static const int emg_window_length_ = 200;

    // CLASSIFICATION
    int pred_class_label_ = 0;
    int classifier_result_;
    std::vector<std::vector<double>> lda_classifier_;
    std::vector<std::vector<double>> lda_intercept_;
    std::vector<std::vector<int>> sel_feats_;
    Eigen::MatrixXd lda_class_eig_;
    Eigen::VectorXd lda_inter_eig_;
    Eigen::VectorXd lda_dist_eig_;

    // STATE TRANSITION EVENT VARIABLES
    bool menu_selected_ = false;
    bool scene_selected_ = false;
    bool init_backdrive_time_reached_ = false;
    bool rps_init_ = false;
    bool target_reached_ = false;
    bool hold_target_time_reached_ = false;
    bool target_input_ = false;
    bool force_mag_reached_ = false;
    bool emg_data_processed_ = false;
    bool end_of_target_sequence_ = false;
    int lda_training_complete_ = 0;

    // TEMPORARY EMG DATA AND FEATURE CONTAINERS
    double_vec emg_voltages_ = double_vec(meii_.N_emg_, 0.0);
    double_vec filtered_emg_voltages_ = double_vec(meii_.N_emg_, 0.0);
    double_vec feature_vec_ = double_vec(num_features_ * meii_.N_emg_, 0.0);
    std::array<double, num_features_ * num_emg_channels_> feature_array_;
    exo::MahiExoIIEmg::EmgDataBuffer emg_data_buffer_ = exo::MahiExoIIEmg::EmgDataBuffer(meii_.N_emg_, emg_window_length_);
    int num_class_;
    std::vector<std::array<double, num_features_ * num_emg_channels_>> emg_training_data_;
    size_t N_train_;
    std::array<int, 2> training_data_size2_;

    // UTILITY FUNCTIONS
    void set_experiment_conditions(int scene_num);
    void set_viz_target_num(int class_label);
    bool is_single_dof();
    bool is_training();
    bool is_testing();
    bool is_blind();
    bool check_wait_time_reached(double wait_time, double init_time, double current_time) const;
    double measure_task_force(double_vec commanded_torques, int target_num, int dof, int condition) const;
    bool check_force_mag_reached(double force_mag_goal, double force_mag);
    void read_csv(std::string filename, std::vector<std::vector<double>>& output);
    void read_csv(std::string filename, std::vector<std::vector<int>>& output);

    // USER INPUT CONTROL
    int check_for_user_input_target();
    bool check_stop();
    bool stop_ = false;

    // EMG FEATURE EXTRACTION FUNCTIONS
    double_vec feature_extract(exo::MahiExoIIEmg::EmgDataBuffer& emg_data_buffer);
    double rms_feature_extract(boost::circular_buffer<double> emg_channel_buffer);
    double mav_feature_extract(boost::circular_buffer<double> emg_channel_buffer);
    double wl_feature_extract(boost::circular_buffer<double> emg_channel_buffer);
    double zc_feature_extract(boost::circular_buffer<double> emg_channel_buffer);
    double ssc_feature_extract(boost::circular_buffer<double> emg_channel_buffer);
    void ar4_feature_extract(double_vec& coeffs, const double_vec& emg_channel_buffer);

    // PYTHON COMMUNICATION
    comm::MelShare trng_size_ = comm::MelShare("trng_size");
    comm::MelShare trng_share_ = comm::MelShare("trng_share", 16384);
    comm::MelShare label_share_ = comm::MelShare("label_share");
    comm::MelShare lda_coeff_ = comm::MelShare("LDA_coeff", 2048);
    comm::MelShare trng_size2_ = comm::MelShare("trng_size2");
    comm::MelShare feat_id_ = comm::MelShare("feat_id");
    comm::MelShare lda_training_flag_ = comm::MelShare("lda_training_flag");

    // MELSCOPE VARIABLES
    comm::MelShare pos_share_ = comm::MelShare("pos_share");
    comm::MelShare vel_share_ = comm::MelShare("vel_share");
    comm::MelShare emg_share_ = comm::MelShare("emg_share");
    comm::MelShare torque_share_ = comm::MelShare("torque_share");

};