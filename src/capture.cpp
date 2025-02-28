#include "spinnaker_sdk_camera_driver/capture.h"
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS(acquisition::Capture, nodelet::Nodelet)

acquisition::Capture::~Capture(){

    // destructor
    ROS_DEBUG("Capture destructor started");
    ifstream file(dump_img_.c_str());
    if (file)
        if (remove(dump_img_.c_str()) != 0)
            ROS_WARN_STREAM("Unable to remove dump image!");

    end_acquisition();
    deinit_cameras();

    // pCam = nullptr;
    
    ROS_INFO_STREAM("Clearing camList...");
    camList_.Clear();

    ROS_INFO_STREAM("Releasing camera pointers...");
    cams.clear();

    ROS_INFO_STREAM("Releasing system instance...");
    system_->ReleaseInstance();

    delete dynamicReCfgServer_;
    // for boost thread
    if (pubThread_) {
        pubThread_->interrupt();
        pubThread_->join();
    }
    // reset pubThread_
    pubThread_.reset();
    //reset it_
    it_.reset();

}

acquisition::Capture::Capture() {
    //
}

void acquisition::Capture::onInit() {
    NODELET_INFO("Initializing nodelet");
    nh_ = this->getNodeHandle();
    nh_pvt_ = this->getPrivateNodeHandle();
    //it_.reset(new image_transport::ImageTransport(nh_));
    it_ = std::shared_ptr<image_transport::ImageTransport>(new image_transport::ImageTransport(nh_));
    // set values to global class variables and register pub, sub to ros
    init_variables_register_to_ros();
    init_array();
    // calling capture::run() in a different thread
    pubThread_.reset(new boost::thread(boost::bind(&acquisition::Capture::run, this)));
    NODELET_INFO("onInit Initialized");
}

void acquisition::Capture::init_variables_register_to_ros() {

    // this is all stuff in previous contructor
    // except for nodehandles assigned in onInit()
    int mem;
    ifstream usb_mem("/sys/module/usbcore/parameters/usbfs_memory_mb");
    if (usb_mem) {
        usb_mem >> mem;
        if (mem >= 1000)
            ROS_INFO_STREAM("[ OK ] USB memory: "<<mem<<" MB");
        else{
            ROS_FATAL_STREAM("  USB memory on system too low ("<<mem<<" MB)! Must be at least 1000 MB. Run: \nsudo sh -c \"echo 1000 > /sys/module/usbcore/parameters/usbfs_memory_mb\"\n Terminating...");
            ros::shutdown();
        }
    } else {
        ROS_FATAL_STREAM("Could not check USB memory on system! Terminating...");
        ros::shutdown();
    }

    // default values for the parameters are set here. Should be removed eventually!!
    exposure_time_ = 0 ; // default as 0 = auto exposure
    soft_framerate_ = 20; //default soft framrate
    gain_ = 0;
    ext_ = ".bmp";
    SOFT_FRAME_RATE_CTRL_ = false;
    LIVE_ = false;
    TIME_BENCHMARK_ = false;
    MASTER_TIMESTAMP_FOR_ALL_ = true;
    EXTERNAL_TRIGGER_ = false;
    CODE_TRIGGER_ = false;
    trigger_capture_ = false;
    EXPORT_TO_ROS_ = false;
    PUBLISH_CAM_INFO_ = false;
    SAVE_ = false;
    SAVE_BIN_ = false;
    nframes_ = -1;
    FIXED_NUM_FRAMES_ = false;
    MAX_RATE_SAVE_ = false;
    region_of_interest_set_ = false;
    skip_num_ = 20;
    init_delay_ = 1;
    master_fps_ = 20.0;
    binning_ = 1;
    SPINNAKER_GET_NEXT_IMAGE_TIMEOUT_ = 2000;
    todays_date_ = todays_date();
    

    first_image_received = false;

    dump_img_ = "dump" + ext_;

    grab_time_ = 0;
    save_time_ = 0;
    toMat_time_ = 0;
    save_mat_time_ = 0;
    export_to_ROS_time_ = 0;
    achieved_time_ = 0;

    // decimation_ = 1;

    CAM_ = 0;

    // default flag values

    MANUAL_TRIGGER_ = false;
    CAM_DIRS_CREATED_ = false;

    GRID_CREATED_ = false;
    VERIFY_BINNING_ = false;
    
    //GPS Data
    nmea_trigger = boost::none;

    //read_settings(config_file);
    read_parameters();

    // Retrieve singleton reference to system object
    ROS_INFO_STREAM("*** SYSTEM INFORMATION ***");
    ROS_INFO_STREAM("Creating system instance...");
    ROS_INFO_STREAM("spinnaker_sdk_camera_driver package version: " << spinnaker_sdk_camera_driver_VERSION);
    system_ = System::GetInstance();
    
    const LibraryVersion spinnakerLibraryVersion = system_->GetLibraryVersion();
    ROS_INFO_STREAM("Spinnaker library version: "
                    << spinnakerLibraryVersion.major << "."
                    << spinnakerLibraryVersion.minor << "."
                    << spinnakerLibraryVersion.type << "."
                    << spinnakerLibraryVersion.build);
 
    software_trigger_sub_ = nh_.subscribe("/ImageCollection/software_trigger", 1000, &acquisition::Capture::assignSoftwareTriggerCallback,this);

    load_cameras();

    //initializing the ros publisher
    acquisition_pub = nh_.advertise<spinnaker_sdk_camera_driver::SpinnakerImageNames>("camera", 1000);
    
    
    //dynamic reconfigure
    dynamicReCfgServer_ = new dynamic_reconfigure::Server<spinnaker_sdk_camera_driver::spinnaker_camConfig>(nh_pvt_);
    
    dynamic_reconfigure::Server<spinnaker_sdk_camera_driver::spinnaker_camConfig>::CallbackType dynamicReCfgServerCB_t;   

    dynamicReCfgServerCB_t = boost::bind(&acquisition::Capture::dynamicReconfigureCallback,this, _1, _2);
    dynamicReCfgServer_->setCallback(dynamicReCfgServerCB_t);

}
void acquisition::Capture::load_cameras() {

    // Retrieve list of cameras from the system
    ROS_INFO_STREAM("Retreiving list of cameras...");
    camList_ = system_->GetCameras();
    
    numCameras_ = camList_.GetSize();
    ROS_ASSERT_MSG(numCameras_,"No cameras found!");
    ROS_INFO_STREAM("Numer of cameras found: " << numCameras_);
    ROS_INFO_STREAM(" Cameras connected: " << numCameras_);

    for (int i=0; i<numCameras_; i++) {
        acquisition::Camera cam(camList_.GetByIndex(i));
        ROS_INFO_STREAM("  -"<< cam.get_id()
                             <<" "<< cam.getTLNodeStringValue("DeviceModelName")
                             <<" "<< cam.getTLNodeStringValue("DeviceVersion") );
    }

    bool master_set = false;
    int cam_counter = 0;
    
    for (int j=0; j<cam_ids_.size(); j++) {
        bool current_cam_found=false;
        for (int i=0; i<numCameras_; i++) {
        
            acquisition::Camera cam(camList_.GetByIndex(i));
            if (!EXTERNAL_TRIGGER_ and !CODE_TRIGGER_){
                cam.setGetNextImageTimeout(SPINNAKER_GET_NEXT_IMAGE_TIMEOUT_);  // set to finite number when not using external triggering
            }

            if (cam.get_id().compare(cam_ids_[j]) == 0) {
                current_cam_found=true;
                if (cam.get_id().compare(master_cam_id_) == 0) {
                    cam.make_master();
                    master_set = true;
                    MASTER_CAM_ = cam_counter;
                }
                
                ImagePtr a_null;
                pResultImages_.push_back(a_null);

                Mat img;
                frames_.push_back(img);
                time_stamps_.push_back("");
        
                cams.push_back(cam);
                
                camera_image_pubs.push_back(it_->advertiseCamera("camera_array/"+cam_names_[j]+"/image_raw", 1));
                camera_image_gps_pubs.push_back(nh_.advertise<msgs_and_srvs::GpsTaggedImageMsg>("camera_array/"+cam_names_[j]+"/gps_image",1,true));
                camera_fps_pub = nh_.advertise<std_msgs::Float64>("camera_array/camera_fps",1,true);
                benchmark_pubs.push_back(nh_.advertise<msgs_and_srvs::CollectionBenchmarkMsg>("camera_array/"+cam_names_[j]+"/benchmark",1,true));

                img_msgs.push_back(sensor_msgs::ImagePtr());

                sensor_msgs::CameraInfoPtr ci_msg(new sensor_msgs::CameraInfo());

                //int image_width = 0;
                //int image_height = 0;
                nh_pvt_.getParam("image_height", image_height_);
                nh_pvt_.getParam("image_width", image_width_);
                // full resolution image_size
                ci_msg->height = image_height_;
                ci_msg->width = image_width_;
                                
                std::string distortion_model = ""; 
                nh_pvt_.getParam("distortion_model", distortion_model);

                // distortion
                ci_msg->distortion_model = distortion_model;
                // binning
                ci_msg->binning_x = binning_;
                ci_msg->binning_y = binning_;
                
                if (region_of_interest_set_ && (region_of_interest_width_!=0 || region_of_interest_height_!=0)){
                    ci_msg->roi.do_rectify = true;
                    ci_msg->roi.width = region_of_interest_width_;
                    ci_msg->roi.height = region_of_interest_height_;
                    ci_msg->roi.x_offset = region_of_interest_x_offset_;
                    ci_msg->roi.y_offset = region_of_interest_y_offset_;
                }
            
                if (PUBLISH_CAM_INFO_){
                    ci_msg->D = distortion_coeff_vec_[j];
                    // intrinsic coefficients
                    for (int count = 0; count<intrinsic_coeff_vec_[j].size();count++){
                        ci_msg->K[count] = intrinsic_coeff_vec_[j][count];
                    }
                    // Rectification matrix
                    if (!rect_coeff_vec_.empty()) 
                        ci_msg->R = {
                            rect_coeff_vec_[j][0], rect_coeff_vec_[j][1], 
                            rect_coeff_vec_[j][2], rect_coeff_vec_[j][3], 
                            rect_coeff_vec_[j][4], rect_coeff_vec_[j][5], 
                            rect_coeff_vec_[j][6], rect_coeff_vec_[j][7], 
                            rect_coeff_vec_[j][8]};
                    // Projection/camera matrix
                    if (!proj_coeff_vec_.empty()){
                        ci_msg->P = {
                            proj_coeff_vec_[j][0], proj_coeff_vec_[j][1], 
                            proj_coeff_vec_[j][2], proj_coeff_vec_[j][3], 
                            proj_coeff_vec_[j][4], proj_coeff_vec_[j][5], 
                            proj_coeff_vec_[j][6], proj_coeff_vec_[j][7], 
                            proj_coeff_vec_[j][8], proj_coeff_vec_[j][9], 
                            proj_coeff_vec_[j][10], proj_coeff_vec_[j][11]};
                    }
                    //else if(numCameras_ == 1){
                    else if(cam_ids_.size() == 1){  
                        // for case of monocular camera, P[1:3,1:3]=K
                        ci_msg->P = {
                        intrinsic_coeff_vec_[j][0], intrinsic_coeff_vec_[j][1],
                        intrinsic_coeff_vec_[j][2], 0, 
                        intrinsic_coeff_vec_[j][3], intrinsic_coeff_vec_[j][4],
                        intrinsic_coeff_vec_[j][5], 0, 
                        intrinsic_coeff_vec_[j][6], intrinsic_coeff_vec_[j][7],
                        intrinsic_coeff_vec_[j][8], 0};
                    }
                }

                cam_info_msgs.push_back(ci_msg);
                cam_counter++;
            }
        }
        if (!current_cam_found) ROS_WARN_STREAM("   Camera "<<cam_ids_[j]<<" not detected!!!");
    }
    ROS_ASSERT_MSG(cams.size(),"None of the connected cameras are in the config list!");

    // Setting numCameras_ variable to reflect number of camera objects used.
    // numCameras_ variable is used in other methods where it means size of cams list.
    numCameras_ = cams.size();
    // setting PUBLISH_CAM_INFO_ to true so export to ros method can publish it_.advertiseCamera msg with zero intrisics and distortion coeffs.
    PUBLISH_CAM_INFO_ = true;

    if (!EXTERNAL_TRIGGER_)
        ROS_ASSERT_MSG(master_set,"The camera supposed to be the master isn't connected!");
}

void acquisition::Capture::read_parameters() {

    ROS_INFO_STREAM("*** PARAMETER SETTINGS ***");
    ROS_INFO_STREAM("** Date = "<<todays_date_);
    
    if (nh_pvt_.getParam("save_path", path_)){
    if(path_.front() =='~'){
        const char *homedir;
        if ((homedir = getenv("HOME")) == NULL)
            homedir = getpwuid(getuid())->pw_dir;
        std::string hd(homedir);
        path_.replace(0,1,hd);
    }
    ROS_INFO_STREAM("  Save path set via parameter to: " << path_);
    }
    else {
    boost::filesystem::path canonicalPath = boost::filesystem::canonical(".", boost::filesystem::current_path());
    path_ = canonicalPath.string();
       
    ROS_WARN_STREAM("  Save path not provided, data will be saved to: " << path_);
    }

    if (path_.back() != '/')
        path_ = path_ + '/';
        
    struct stat sb;
    ROS_ASSERT_MSG(stat(path_.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode),"Specified Path Doesn't Exist!!!");

    ROS_INFO("  Camera IDs:");
    
    std::vector<int> cam_id_vec;
    ROS_ASSERT_MSG(nh_pvt_.getParam("cam_ids", cam_id_vec),"If cam_aliases are provided, they should be the same number as cam_ids and should correspond in order!");
    int num_ids = cam_id_vec.size();
    for (int i=0; i < num_ids; i++){
        cam_ids_.push_back(to_string(cam_id_vec[i]));
        ROS_INFO_STREAM("    " << to_string(cam_id_vec[i]));
    }

    std::vector<string> cam_alias_vec;
    if (nh_pvt_.getParam("cam_aliases", cam_names_)){
        ROS_INFO_STREAM("  Camera Aliases:");
        ROS_ASSERT_MSG(num_ids == cam_names_.size(),"If cam_aliases are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<cam_names_.size(); i++) {
            ROS_INFO_STREAM("    " << cam_ids_[i] << " >> " << cam_names_[i]);
        }
    } else {
        ROS_INFO_STREAM("  No camera aliases provided. Camera IDs will be used as names.");
        for (int i=0; i<cam_ids_.size(); i++)
            cam_names_.push_back(cam_ids_[i]);
    }
    
    if (nh_pvt_.getParam("external_trigger", EXTERNAL_TRIGGER_)){
        ROS_INFO("  External trigger: %s",EXTERNAL_TRIGGER_?"true":"false");
    }
    if (nh_pvt_.getParam("code_trigger", CODE_TRIGGER_)){
        ROS_INFO("  Code trigger: %s",CODE_TRIGGER_?"true":"false");
    }
    else ROS_WARN("  'external_trigger' Parameter not set, using default behavior external_trigger=%s",EXTERNAL_TRIGGER_?"true":"false");


    // Unless external trigger is being used, a master cam needs to be specified
    // If the external trigger is set, all the cameras are set up as slave    int mcam_int;

    int mcam_int;

    if (!EXTERNAL_TRIGGER_){
      ROS_ASSERT_MSG(nh_pvt_.getParam("master_cam", mcam_int),"master_cam is required!");
    
      master_cam_id_=to_string(mcam_int);
      bool found = false;
      for (int i=0; i<cam_ids_.size(); i++) {
          if (master_cam_id_.compare(cam_ids_[i]) == 0)
              found = true;
      }
      ROS_ASSERT_MSG(found,"Specified master cam is not in the cam_ids list!");
    }
	
    if (nh_pvt_.getParam("utstamps", MASTER_TIMESTAMP_FOR_ALL_)){
        MASTER_TIMESTAMP_FOR_ALL_ = !MASTER_TIMESTAMP_FOR_ALL_;
        ROS_INFO("  Unique time stamps for each camera: %s",!MASTER_TIMESTAMP_FOR_ALL_?"true":"false");
    } 
        else ROS_WARN("  'utstamps' Parameter not set, using default behavior utstamps=%s",!MASTER_TIMESTAMP_FOR_ALL_?"true":"false");
    
    if (nh_pvt_.getParam("color", color_)) 
        ROS_INFO("  color set to: %s",color_?"true":"false");
        else ROS_WARN("  'color' Parameter not set, using default behavior color=%s",color_?"true":"false");
        
    if (nh_pvt_.getParam("flip_horizontal", flip_horizontal_vec_)){
        ROS_ASSERT_MSG(num_ids == flip_horizontal_vec_.size(),"If flip_horizontal flags are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<flip_horizontal_vec_.size(); i++) {
            ROS_INFO_STREAM("  "<<cam_ids_[i] << " flip_horizontal " << flip_horizontal_vec_[i]);
        }
    }
    else {
        ROS_WARN_STREAM("  flip_horizontal flags are not provided. default behavior is false ");
        for (int i=0; i<cam_ids_.size(); i++){
            flip_horizontal_vec_.push_back(false);
            ROS_WARN_STREAM("  "<<cam_ids_[i] << " flip_horizontal set to default = " << flip_horizontal_vec_[i]);
        }
    }

    if (nh_pvt_.getParam("flip_vertical", flip_vertical_vec_)){
        ROS_ASSERT_MSG(num_ids == flip_vertical_vec_.size(),"If flip_vertical flags are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<flip_vertical_vec_.size(); i++) {
            ROS_INFO_STREAM("  "<<cam_ids_[i] << " flip_vertical " << flip_vertical_vec_[i]);
        }
    }
    else {
        ROS_WARN_STREAM("  flip_vertical flags are not provided. default behavior is false ");
        for (int i=0; i<cam_ids_.size(); i++){
            flip_vertical_vec_.push_back(false);
            ROS_WARN_STREAM("  "<<cam_ids_[i] << " flip_vertical set to default = " << flip_vertical_vec_[i]);
        }
    }

    if (nh_pvt_.getParam("to_ros", EXPORT_TO_ROS_)) 
        ROS_INFO("  Exporting images to ROS: %s",EXPORT_TO_ROS_?"true":"false");
        else ROS_WARN("  'to_ros' Parameter not set, using default behavior to_ros=%s",EXPORT_TO_ROS_?"true":"false");

    if (nh_pvt_.getParam("live", LIVE_)) 
        ROS_INFO("  Showing live images setting: %s",LIVE_?"true":"false");
        else ROS_WARN("  'live' Parameter not set, using default behavior live=%s",LIVE_?"true":"false");

    if (nh_pvt_.getParam("live_grid", GRID_VIEW_)){
        LIVE_=LIVE_|| GRID_VIEW_;
        ROS_INFO("  Showing grid-style live images setting: %s",GRID_VIEW_?"true":"false");
    } else ROS_WARN("  'live_grid' Parameter not set, using default behavior live_grid=%s",GRID_VIEW_?"true":"false");

    if (nh_pvt_.getParam("max_rate_save", MAX_RATE_SAVE_)) 
        ROS_INFO("  Max Rate Save Mode: %s",MAX_RATE_SAVE_?"true":"false");
        else ROS_WARN("  'max_rate_save' Parameter not set, using default behavior max_rate_save=%s",MAX_RATE_SAVE_?"true":"false");

    if (nh_pvt_.getParam("time", TIME_BENCHMARK_)) 
        ROS_INFO("  Displaying timing details: %s",TIME_BENCHMARK_?"true":"false");
        else ROS_WARN("  'time' Parameter not set, using default behavior time=%s",TIME_BENCHMARK_?"true":"false");

    if (nh_pvt_.getParam("skip", skip_num_)){
        if (skip_num_ >0) ROS_INFO("  No. of images to skip set to: %d",skip_num_);
        else {
            skip_num_=20;
            ROS_WARN("  Provided 'skip' is not valid, using default behavior, skip=%d",skip_num_);
        }
    } else ROS_WARN("  'skip' Parameter not set, using default behavior: skip=%d",skip_num_);

    if (nh_pvt_.getParam("delay", init_delay_)){
        if (init_delay_>=0) ROS_INFO("  Init sleep delays set to : %0.2f sec",init_delay_);
        else {
            init_delay_=1;
            ROS_WARN("  Provided 'delay' is not valid, using default behavior, delay=%f",init_delay_);
        }
    } else ROS_WARN("  'delay' Parameter not set, using default behavior: delay=%f",init_delay_);

    if (nh_pvt_.getParam("fps", master_fps_)){
        if (master_fps_>=0) ROS_INFO("  Master cam fps set to : %0.2f",master_fps_);
        else {
            master_fps_=20;
            ROS_WARN("  Provided 'fps' is not valid, using default behavior, fps=%0.2f",master_fps_);
        }
    }
        else ROS_WARN("  'fps' Parameter not set, using default behavior: fps=%0.2f",master_fps_);

    if (nh_pvt_.getParam("exposure_time", exposure_time_)){
        if (exposure_time_ >0) ROS_INFO("  Exposure set to: %.1f",exposure_time_);
        else ROS_INFO("  'exposure_time'=%0.f, Setting autoexposure",exposure_time_);
    } else ROS_WARN("  'exposure_time' Parameter not set, using default behavior: Automatic Exposure ");

    if(nh_pvt_.getParam("gain", gain_)){
       if(gain_>0){
          ROS_INFO("gain value set to:%.1f",gain_);
       }
       else ROS_INFO("  'gain' Parameter was zero or negative, using Auto gain based on target grey value");
    } 
    else ROS_WARN("  'gain' Parameter not set, using default behavior: Auto gain based on target grey value");

    if (nh_pvt_.getParam("target_grey_value", target_grey_value_)){
        if (target_grey_value_ >0) ROS_INFO("  target_grey_value set to: %.1f",target_grey_value_);
        else ROS_INFO("  'target_grey_value'=%0.f, Setting AutoExposureTargetGreyValueAuto to Continuous/ auto",target_grey_value_);} 
    else ROS_WARN("  'target_grey_value' Parameter not set, using default behavior: AutoExposureTargetGreyValueAuto to auto");


    if (nh_pvt_.getParam("binning", binning_)){
        if (binning_ >0) ROS_INFO("  Binning set to: %d",binning_);
        else {
            binning_=1;
            ROS_INFO("  'binning'=%d invalid, Using defauly binning=",binning_);
        }
    } else ROS_WARN("  'binning' Parameter not set, using default behavior: Binning = %d",binning_);

    if (nh_pvt_.getParam("soft_framerate", soft_framerate_)){
        if (soft_framerate_ >0) {
            SOFT_FRAME_RATE_CTRL_=true;
            ROS_INFO("  Using Software rate control, rate set to: %d",soft_framerate_);
        }
        else ROS_INFO("  'soft_framerate'=%d, software rate control set to off",soft_framerate_);
    }
    else ROS_WARN("  'soft_framerate' Parameter not set, using default behavior: No Software Rate Control ");

    if (nh_pvt_.getParam("save", SAVE_)) 
        ROS_INFO("  Saving images set to: %d",SAVE_);
        else ROS_WARN("  'save' Parameter not set, using default behavior save=%d",SAVE_);

    if (SAVE_||LIVE_){
        if (nh_pvt_.getParam("save_type", ext_)){
            if (ext_.compare("bin") == 0) SAVE_BIN_ = true;
            ROS_INFO_STREAM("    save_type set as: "<<ext_);
            ext_="."+ext_;
        }else ROS_WARN("    'save_type' Parameter not set, using default behavior save=%d",SAVE_);
    }

    if (SAVE_||MAX_RATE_SAVE_){
        if (nh_pvt_.getParam("frames", nframes_)) {
            if (nframes_>0){
                FIXED_NUM_FRAMES_ = true;
                ROS_INFO("    Number of frames to be recorded: %d", nframes_ );
            }else ROS_INFO("    Frames will be recorded until user termination");
        }else ROS_WARN("    'frames' Parameter not set, using defult behavior, frames will be recorded until user termination");
    }
    
    if (nh_pvt_.hasParam("tf_prefix")){
        nh_pvt_.param<std::string>("tf_prefix", tf_prefix_, "");
        ROS_INFO_STREAM("  tf_prefix set to: "<<tf_prefix_);
    }
    else ROS_WARN("  'tf_prefix' Parameter not set, using default behavior tf_prefix=" " ");

    if (nh_pvt_.hasParam("region_of_interest")){
        region_of_interest_set_ = true;
        if (!nh_pvt_.getParam("region_of_interest/width", region_of_interest_width_)){
            region_of_interest_set_ = false;
            }
        if (!nh_pvt_.getParam("region_of_interest/height", region_of_interest_height_)){
            region_of_interest_set_ = false;
            }
        if (!nh_pvt_.getParam("region_of_interest/x_offset", region_of_interest_x_offset_)){
            region_of_interest_set_ = false;
            }
        if (!nh_pvt_.getParam("region_of_interest/y_offset", region_of_interest_y_offset_)){
            region_of_interest_set_ = false;
            }
        
        if (region_of_interest_set_){
            ROS_INFO("  Region of Interest set to width: %d\theight: %d\toffset_x: %d offset_y: %d",
                     region_of_interest_width_, region_of_interest_height_, region_of_interest_x_offset_, region_of_interest_y_offset_);
        } else ROS_ERROR("  'region_of_interest' Parameter found but not configured correctly, NOT BEING USED");
    } else ROS_INFO_STREAM("  'region of interest' not set using full resolution");

    bool intrinsics_list_provided = false;
    XmlRpc::XmlRpcValue intrinsics_list;
    if (nh_pvt_.getParam("intrinsic_coeffs", intrinsics_list)) {
        ROS_INFO("  Camera Intrinsic Paramters:");
        ROS_ASSERT_MSG(intrinsics_list.size() == num_ids,"If intrinsic_coeffs are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<intrinsics_list.size(); i++){
            std::vector<double> intrinsics;
            String intrinsics_str="";
            for (int j=0; j<intrinsics_list[i].size(); j++){
                ROS_ASSERT_MSG(intrinsics_list[i][j].getType()== XmlRpc::XmlRpcValue::TypeDouble,"Make sure all numbers are entered as doubles eg. 0.0 or 1.1");
                intrinsics.push_back(static_cast<double>(intrinsics_list[i][j]));
                intrinsics_str = intrinsics_str +to_string(intrinsics[j])+" ";
            }

            intrinsic_coeff_vec_.push_back(intrinsics);
            ROS_INFO_STREAM("   "<< intrinsics_str );
            intrinsics_list_provided=true;
        }
    }
    bool distort_list_provided = false;
    XmlRpc::XmlRpcValue distort_list;

    if (nh_pvt_.getParam("distortion_coeffs", distort_list)) {
        ROS_INFO("  Camera Distortion Paramters:");
        ROS_ASSERT_MSG(distort_list.size() == num_ids,"If intrinsic_coeffs are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<distort_list.size(); i++){
            std::vector<double> distort;
            String distort_str="";
            for (int j=0; j<distort_list[i].size(); j++){
                ROS_ASSERT_MSG(distort_list[i][j].getType()== XmlRpc::XmlRpcValue::TypeDouble,"Make sure all numbers are entered as doubles eg. 0.0 or 1.1");
                distort.push_back(static_cast<double>(distort_list[i][j]));
                distort_str = distort_str +to_string(distort[j])+" ";
            }
            distortion_coeff_vec_.push_back(distort);
            ROS_INFO_STREAM("   "<< distort_str );
            distort_list_provided = true;
        }
    }
    
    XmlRpc::XmlRpcValue rect_list;

    if (nh_pvt_.getParam("rectification_coeffs", rect_list)) {
        ROS_INFO("  Camera Rectification Paramters:");
        ROS_ASSERT_MSG(rect_list.size() == num_ids,"If rectification_coeffs are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<rect_list.size(); i++){
            std::vector<double> rect;
            String rect_str="";
            for (int j=0; j<rect_list[i].size(); j++){
                ROS_ASSERT_MSG(rect_list[i][j].getType()== XmlRpc::XmlRpcValue::TypeDouble,"Make sure all numbers are entered as doubles eg. 0.0 or 1.1");
                rect.push_back(static_cast<double>(rect_list[i][j]));
                rect_str = rect_str +to_string(rect[j])+" ";
            }
            rect_coeff_vec_.push_back(rect);
            ROS_INFO_STREAM("   "<< rect_str );
        }
    }
    
    XmlRpc::XmlRpcValue proj_list;

    if (nh_pvt_.getParam("projection_coeffs", proj_list)) {
        ROS_INFO("  Camera Projection Paramters:");
        ROS_ASSERT_MSG(proj_list.size() == num_ids,"If projection_coeffs are provided, they should be the same number as cam_ids and should correspond in order!");
        for (int i=0; i<proj_list.size(); i++){
            std::vector<double> proj;
            String proj_str="";
            for (int j=0; j<proj_list[i].size(); j++){
                ROS_ASSERT_MSG(proj_list[i][j].getType()== XmlRpc::XmlRpcValue::TypeDouble,"Make sure all numbers are entered as doubles eg. 0.0 or 1.1");
                proj.push_back(static_cast<double>(proj_list[i][j]));
                proj_str = proj_str +to_string(proj[j])+" ";
            }
            proj_coeff_vec_.push_back(proj);
            ROS_INFO_STREAM("   "<< proj_str );
        }
    }

    PUBLISH_CAM_INFO_ = intrinsics_list_provided && distort_list_provided;
    if (PUBLISH_CAM_INFO_)
        ROS_INFO("  Camera coeffs provided, camera info messges will be published.");
    else
        ROS_WARN("  Camera coeffs not provided correctly, camera info messges intrinsics and distortion coeffs will be published with zeros.");

//    ROS_ASSERT_MSG(my_list.getType()
//    int num_ids = cam_id_vec.size();
//    for (int i=0; i < num_ids; i++){
//        cam_ids_.push_back(to_string(cam_id_vec[i]));
//        ROS_INFO_STREAM("    " << to_string(cam_id_vec[i]));
//    }

}


void acquisition::Capture::init_array() {
    
    ROS_INFO_STREAM("*** FLUSH SEQUENCE ***");

    init_cameras(true);

    start_acquisition();
    sleep(init_delay_*0.5);

    end_acquisition();
    sleep(init_delay_*0.5);

    deinit_cameras();
    sleep(init_delay_*2.0);

    init_cameras(false);

    ROS_DEBUG_STREAM("Flush sequence done.");

}

void acquisition::Capture::init_cameras(bool soft = false) {

    ROS_INFO_STREAM("Initializing cameras...");
    
    // Set cameras 1 to 4 to continuous
    for (int i = numCameras_-1 ; i >=0 ; i--) {
                                
        ROS_DEBUG_STREAM("Initializing camera " << cam_ids_[i] << "...");

        try {
            
            cams[i].init();

            if (!soft) {
                cams[i].setBufferSize(100);
                cams[i].set_color(color_);
                cams[i].setIntValue("BinningHorizontal", binning_);
                cams[i].setIntValue("BinningVertical", binning_);                
                cams[i].setEnumValue("ExposureMode", "Timed");
                cams[i].setBoolValue("ReverseX", flip_horizontal_vec_[i]);
                cams[i].setBoolValue("ReverseY", flip_vertical_vec_[i]);
                
                if (region_of_interest_set_){
                    if (region_of_interest_width_ != 0)
                        cams[i].setIntValue("Width", region_of_interest_width_);
                    if (region_of_interest_height_ != 0)
                        cams[i].setIntValue("Height", region_of_interest_height_);
                    cams[i].setIntValue("OffsetX", region_of_interest_x_offset_);
                    cams[i].setIntValue("OffsetY", region_of_interest_y_offset_);
                }
                
                if (exposure_time_ > 0) { 
                    cams[i].setEnumValue("ExposureAuto", "Off");
                    cams[i].setFloatValue("ExposureTime", exposure_time_);
                } else {
                    cams[i].setEnumValue("ExposureAuto", "Continuous");
                }
                
                if(gain_>0){ //fixed gain
                    cams[i].setEnumValue("GainAuto", "Off");
                    double max_gain_allowed = cams[i].getFloatValueMax("Gain");
                    if (gain_ <= max_gain_allowed)
                        cams[i].setFloatValue("Gain", gain_);
                    else {
                        cams[i].setFloatValue("Gain", max_gain_allowed);
                        ROS_WARN("Provided Gain value is higher than max allowed, setting gain to %f", max_gain_allowed);
                    }
                    target_grey_value_ = 50;
                } else {
                    cams[i].setEnumValue("GainAuto","Continuous");                   
                }

                if (target_grey_value_ > 4.0) {
                    cams[i].setEnumValue("AutoExposureTargetGreyValueAuto", "Off");
                    cams[i].setFloatValue("AutoExposureTargetGreyValue", target_grey_value_);
                } else {
                    cams[i].setEnumValue("AutoExposureTargetGreyValueAuto", "Continuous");
                }

                // cams[i].setIntValue("DecimationHorizontal", decimation_);
                // cams[i].setIntValue("DecimationVertical", decimation_);
                // cams[i].setFloatValue("AcquisitionFrameRate", 5.0);

                if (color_)
                        cams[i].setEnumValue("PixelFormat", "BayerRG8");
                    else
                        cams[i].setEnumValue("PixelFormat", "Mono8");
                
                cams[i].setEnumValue("AcquisitionMode", "Continuous");
                
                // set only master to be software triggered
                if (cams[i].is_master()) { 
                    if (MAX_RATE_SAVE_ && !CODE_TRIGGER_){
                      cams[i].setEnumValue("LineSelector", "Line2");
                      cams[i].setEnumValue("LineMode", "Output");
                      cams[i].setBoolValue("AcquisitionFrameRateEnable", false);
                      //cams[i].setFloatValue("AcquisitionFrameRate", 170);
                    } else{
                      cams[i].setEnumValue("TriggerMode", "On");
                      cams[i].setEnumValue("LineSelector", "Line2");
                      cams[i].setEnumValue("LineMode", "Output");
                      cams[i].setEnumValue("TriggerSource", "Software");
                    }
                    //cams[i].setEnumValue("LineSource", "ExposureActive");


                } else{ // sets the configuration for external trigger: used for all slave cameras 
                        // in master slave setup. Also in the mode when another sensor such as IMU triggers 
                        // the camera
                    cams[i].setEnumValue("TriggerMode", "On");
                    cams[i].setEnumValue("LineSelector", "Line3");
                    cams[i].setEnumValue("TriggerSource", "Line3");
                    cams[i].setEnumValue("TriggerSelector", "FrameStart");
                    cams[i].setEnumValue("LineMode", "Input");
                    
//                    cams[i].setFloatValue("TriggerDelay", 40.0);
                    cams[i].setEnumValue("TriggerOverlap", "ReadOut");//"Off"
                    cams[i].setEnumValue("TriggerActivation", "RisingEdge");
                }
            }
        
        }

        catch (Spinnaker::Exception &e) {
            string error_msg = e.what();
            ROS_FATAL_STREAM("Error: " << error_msg);
            if (error_msg.find("Unable to set PixelFormat to BGR8") >= 0)
              ROS_WARN("Most likely cause for this error is if your camera can't support color and your are trying to set it to color mode");
            ros::shutdown();
        }

    }
    ROS_DEBUG_STREAM("All cameras initialized.");
}

void acquisition::Capture::start_acquisition() {

    for (int i = numCameras_-1; i>=0; i--)
        cams[i].begin_acquisition();

    // for (int i=0; i<numCameras_; i++)
    //     cams[i].begin_acquisition();
    
}

void acquisition::Capture::end_acquisition() {

    for (int i = 0; i < numCameras_; i++)
        cams[i].end_acquisition();
    
}

void acquisition::Capture::deinit_cameras() {

    ROS_INFO_STREAM("Deinitializing cameras...");

    // end_acquisition();
    
    for (int i = numCameras_-1 ; i >=0 ; i--) {

        ROS_DEBUG_STREAM("Camera "<<i<<": Deinit...");
        cams[i].deinit();
        // pCam = NULL;
    }
    ROS_INFO_STREAM("All cameras deinitialized."); 

}

void acquisition::Capture::create_cam_directories() {

    ROS_DEBUG_STREAM("Creating camera directories...");
    
    for (int i=0; i<numCameras_; i++) {
        ostringstream ss;
        ss<<path_<<cam_names_[i];
        if (mkdir(ss.str().c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0) {
            ROS_WARN_STREAM("Failed to create directory "<<ss.str()<<"! Data will be written into pre existing directory if it exists...");
        }
    }

    CAM_DIRS_CREATED_ = true;
    
}

void acquisition::Capture::save_mat_frames(int dump) {
    
    double t = ros::Time::now().toSec();

    if (!CAM_DIRS_CREATED_)
        create_cam_directories();
    
    string timestamp;
    for (unsigned int i = 0; i < numCameras_; i++) {

        if (dump) {
            
            imwrite(dump_img_.c_str(), frames_[i]);
            ROS_DEBUG_STREAM("Skipping frame...");
            
        } else {

            if (MASTER_TIMESTAMP_FOR_ALL_)
                timestamp = time_stamps_[MASTER_CAM_];
            else
                timestamp = time_stamps_[i];

            ostringstream filename;
            filename<< path_ << cam_names_[i] << "/" << timestamp << ext_;
            ROS_DEBUG_STREAM("Saving image at " << filename.str());
            //ros image names 
            mesg.name.push_back(filename.str());
            imwrite(filename.str(), frames_[i]);
            try {
                boost::property_tree::ptree ptree;
                ptree.put("camera.easting", 123123123);
                ptree.put("camera.northing", 123123123);
                ptree.put("camera.altitude", 123123123);
                ptree.put("camera.zone", 12);

                std::ofstream file;
	            std::ostringstream oss;

	            boost::property_tree::write_json(oss, ptree);

                Exiv2::ExifData exif_data;
                exif_data["Exif.Image.Model"] = "Test 1";  
                exif_data["Exif.Image.ImageDescription"] = oss.str(); 
                Exiv2::Image::UniquePtr image_exif_file = Exiv2::ImageFactory::open(filename.str());
                image_exif_file->setExifData(exif_data);
				image_exif_file->writeMetadata();

            }
            catch( const Exiv2::AnyError& ex )
            {
                // ROS_ERROR("Could not write the exif data to '%s' -- %s", filename.str(), ex.what());
                ROS_WARN("Could not write the exif data - %s",ex.what());
                
            }
        }

    }
    
    save_mat_time_ = ros::Time::now().toSec() - t;
    
}

void acquisition::Capture::export_to_ROS() {
    double t = ros::Time::now().toSec();
    std_msgs::Header img_msg_header;
    
    
    string frame_id_prefix;
    if (tf_prefix_.compare("") != 0)
        frame_id_prefix = tf_prefix_ +"/";
    else frame_id_prefix="";

    for (unsigned int i = 0; i < numCameras_; i++) {
        img_msg_header.frame_id = frame_id_prefix + "cam_"+to_string(i)+"_optical_frame";
        cam_info_msgs[i]->header = img_msg_header;

        if(color_)
            img_msgs[i]=cv_bridge::CvImage(img_msg_header, "bgr8", frames_[i]).toImageMsg();
        else
            img_msgs[i]=cv_bridge::CvImage(img_msg_header, "mono8", frames_[i]).toImageMsg();

        camera_image_pubs[i].publish(img_msgs[i],cam_info_msgs[i]);

    }
    export_to_ROS_time_ = ros::Time::now().toSec()-t;;
}

void acquisition::Capture::save_binary_frames(int dump) {
    
    double t = ros::Time::now().toSec();

    if (!CAM_DIRS_CREATED_)
        create_cam_directories();
    
    string timestamp;
    for (unsigned int i = 0; i < numCameras_; i++) {

        if (dump) {
            //imwrite(dump_img_.c_str(), frames_[i]);
            ROS_DEBUG_STREAM("Skipping frame...");
        } else {

            if (MASTER_TIMESTAMP_FOR_ALL_)
                timestamp = time_stamps_[MASTER_CAM_];
            else
                timestamp = time_stamps_[i];
                
            ostringstream filename;
            filename<< path_ << cam_names_[i] << "/" << timestamp << ".bin";
            ROS_DEBUG_STREAM("Saving image at " << filename.str());
            //ros image names
            mesg.name.push_back(filename.str());
            std::ofstream ofs(filename.str());
            boost::archive::binary_oarchive oa(ofs);
            oa << frames_[i];
            ofs.close();
            
        }

    }
    save_mat_time_ = ros::Time::now().toSec() - t;
    
}

Mat acquisition::Capture::convert_to_mat(ImagePtr pImage) {

    ImagePtr convertedImage;
    convertedImage = pImage->Convert(PixelFormat_BGR8); //, NEAREST_NEIGHBOR);
    unsigned int XPadding = convertedImage->GetXPadding();
    unsigned int YPadding = convertedImage->GetYPadding();
    unsigned int rowsize = convertedImage->GetWidth();
    unsigned int colsize = convertedImage->GetHeight();
    //image data contains padding. When allocating Mat container size, you need to account for the X,Y image data padding.
    Mat img;
    img = Mat(colsize + YPadding, rowsize + XPadding, CV_8UC3, convertedImage->GetData(), convertedImage->GetStride());
    return img.clone();

    
}

void acquisition::Capture::get_mat_images() {
    //ros time stamp creation
    //mesg.header.stamp = ros::Time::now();
    //mesg.time = ros::Time::now();
    double t = ros::Time::now().toSec();
    
    ostringstream ss;
    ss<<"frameIDs: [";
    
    int frameID;
    int fid_mismatch = 0;
   

    for (int i=0; i<numCameras_; i++) {
        //ROS_INFO_STREAM("CAM ID IS "<< i);
        frames_[i] = cams[i].grab_mat_frame();
        //ROS_INFO("sucess");
        time_stamps_[i] = cams[i].get_time_stamp();


        if (i==0)
            frameID = cams[i].get_frame_id();
        else
            if (cams[i].get_frame_id() != frameID)
                fid_mismatch = 1;
        
        if (i == numCameras_-1)
            ss << cams[i].get_frame_id() << "]";
        else
            ss << cams[i].get_frame_id() << ", ";
        
    }
    mesg.header.stamp = ros::Time::now();
    mesg.time = ros::Time::now();
    string message = ss.str();
    ROS_DEBUG_STREAM(message);

    if (fid_mismatch)
        ROS_WARN_STREAM("Frame IDs for grabbed set of images did not match!");
    
    toMat_time_ = ros::Time::now().toSec() - t;
    
}

void acquisition::Capture::run_soft_trig() {
    achieved_time_ = ros::Time::now().toSec();
    ROS_INFO("*** ACQUISITION ***");
    
    start_acquisition();

    // Camera directories created at first save
    
    if (LIVE_)namedWindow("Acquisition", WINDOW_NORMAL | WINDOW_KEEPRATIO);

    int count = 0;
    
    if (!EXTERNAL_TRIGGER_) {
        cams[MASTER_CAM_].trigger();
    }
    
    get_mat_images();
    if (SAVE_) {
        count++;
        if (SAVE_BIN_)
            save_binary_frames(0);
        else
            save_mat_frames(0);
    }

    if(!VERIFY_BINNING_){
    // Gets called only once, when first image is being triggered
        for (unsigned int i = 0; i < numCameras_; i++) {
            //verify if binning is set successfully
            if (!region_of_interest_set_){
                ROS_ASSERT_MSG(cams[i].verifyBinning(binning_), " Failed to set Binning= %d, could be either due to Invalid binning value, try changing binning value or due to spinnaker api bug - failing to set lower binning than previously set value - solution: unplug usb camera and re-plug it back and run to node with desired valid binning", binning_);
            }
            // warn if full sensor resolution is not same as calibration resolution
            cams[i].calibrationParamsTest(image_width_,image_height_);
        }
    VERIFY_BINNING_ = true;
    }


    ros::Rate ros_rate(soft_framerate_);
    try{
        while( ros::ok() ) {

            double t = ros::Time::now().toSec();

            if (LIVE_) {
                if (GRID_VIEW_) {
                    update_grid();
                    imshow("Acquisition", grid_);
                } else {
                    imshow("Acquisition", frames_[CAM_]);
                    char title[50];
                    sprintf(title, "cam # = %d, cam ID = %s, cam name = %s", CAM_, cam_ids_[CAM_].c_str(), cam_names_[CAM_].c_str());
                    displayOverlay("Acquisition", title);
                }
            }

            int key = waitKey(1);
            ROS_DEBUG_STREAM("Key press: "<<(key & 255)<<endl);
            if ( (key & 255)!=255 ) {

                if ( (key & 255)==83 ) {
                    if (CAM_<numCameras_-1) // RIGHT ARROW
                        CAM_++;
                } else if( (key & 255)==81 ) { // LEFT ARROW
                    if (CAM_>0)
                        CAM_--;
                } else if( (key & 255)==84 && MANUAL_TRIGGER_) { // t
                    cams[MASTER_CAM_].trigger();
                    get_mat_images();
                } else if( (key & 255)==32 && !SAVE_) { // SPACE
                    ROS_INFO_STREAM("Saving frame...");
                    if (SAVE_BIN_)
                        save_binary_frames(0);
                        else{
                            save_mat_frames(0);
                            if (!EXPORT_TO_ROS_){
                                ROS_INFO_STREAM("Exporting frames to ROS...");
                                export_to_ROS();
                            }
                        }
                } else if( (key & 255)==27 ) {  // ESC
                    ROS_INFO_STREAM("Terminating...");
                    destroyAllWindows();
                    ros::shutdown();
                    break;
                }
                ROS_DEBUG_STREAM("active cam switched to: "<<CAM_);
            }

            double disp_time_ = ros::Time::now().toSec() - t;

            // Call update functions
            if (!MANUAL_TRIGGER_) {
                if (!EXTERNAL_TRIGGER_) {
                    if (!CODE_TRIGGER_) { 
                        cams[MASTER_CAM_].trigger();
                        get_mat_images();
                    }else{
                        if (trigger_capture_) {
                             cams[MASTER_CAM_].trigger();
                             get_mat_images();
                             trigger_capture_ = false;
                        }
                    }
                }
                
            }

            if (SAVE_) {
                count++;
                if (SAVE_BIN_)
                    save_binary_frames(0);
                else
                    save_mat_frames(0);
            }

            if (FIXED_NUM_FRAMES_) {
                cout<<"Nframes "<< nframes_<<endl;
                if (count > nframes_) {
                    ROS_INFO_STREAM(nframes_ << " frames recorded. Terminating...");
                    destroyAllWindows();
                    break;
                }
            }

            if (EXPORT_TO_ROS_) export_to_ROS();
            //cams[MASTER_CAM_].targetGreyValueTest();
            // ros publishing messages
            acquisition_pub.publish(mesg);

            // double total_time = grab_time_ + toMat_time_ + disp_time_ + save_mat_time_;
            double total_time = toMat_time_ + disp_time_ + save_mat_time_+export_to_ROS_time_;
            achieved_time_ = ros::Time::now().toSec() - achieved_time_;

            ROS_INFO_COND(TIME_BENCHMARK_,
                          "total time (ms): %.1f \tPossible FPS: %.1f\tActual FPS: %.1f",
                          total_time*1000,1/total_time,1/achieved_time_);
            
            ROS_INFO_COND(TIME_BENCHMARK_,"Times (ms):- grab: %.1f, disp: %.1f, save: %.1f, exp2ROS: %.1f",
                          toMat_time_*1000,disp_time_*1000,save_mat_time_*1000,export_to_ROS_time_*1000);
            
            achieved_time_=ros::Time::now().toSec();
            
            if (SOFT_FRAME_RATE_CTRL_) {ros_rate.sleep();}

        }
    }
    catch(const std::exception &e){
        ROS_FATAL_STREAM("Exception: "<<e.what());
    }
    catch(...){
        ROS_FATAL_STREAM("Some unknown exception occured. \v Exiting gracefully, \n  possible reason could be Camera Disconnection...");
    }
    ros::shutdown();
    //raise(SIGINT);
}

float acquisition::Capture::mem_usage() {
    std::string token;
    std::ifstream file("/proc/meminfo");
    unsigned long int total, free;
    while (file >> token) {
        if (token == "MemTotal:")
            if (!(file >> total))
                ROS_FATAL_STREAM("Could not poll total memory!");
        if (token == "MemAvailable:")
            if (!(file >> free)) {
                ROS_FATAL_STREAM("Could not poll free memory!");
                break;
            }
        // ignore rest of the line
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 1-float(free)/float(total);
}

void acquisition::Capture::update_grid() {

    if (!GRID_CREATED_) {
        int height = frames_[0].rows;
        int width = frames_[0].cols*cams.size();
        
        if (color_)
        grid_.create(height, width, CV_8UC3);
        else
        grid_.create(height, width, CV_8U);
        
        GRID_CREATED_ = true;
    }

    for (int i=0; i<cams.size(); i++)
        frames_[i].copyTo(grid_.colRange(i*frames_[i].cols,i*frames_[i].cols+frames_[i].cols).rowRange(0,grid_.rows));
    
}

//*** CODE FOR MULTITHREADED WRITING
void acquisition::Capture::write_queue_to_disk(queue<Metadata>* img_q, int cam_no) {
    double ml_grab_time_ = 0;
    double ml_save_time_ = 0;
    double ml_toMat_time_ = 0;
    double metadata_write_time_ = 0;
    double ml_export_to_ROS_time_ = 0;

    ROS_DEBUG("  Write Queue to Disk Thread Initiated for cam: %d", cam_no);
    string id = cam_ids_[cam_no];
    int imageCnt =0;
    uint64_t timeStamp = 0;
    try{
        while( ros::ok() ) {
            double t = ros::Time::now().toSec();
            if (img_q->size()== 0)
                continue;

            ROS_DEBUG_STREAM("  Write Queue to Disk for cam: "<< cam_no <<" size = "<<img_q->size());

            if (img_q->size()>100)
                ROS_WARN_STREAM("  Queue "<<cam_no<<" size is :"<< img_q->size());
            
            ImagePtr convertedImage = img_q->front().image;
            msgs_and_srvs::ImageTriggerMsg trigger_message = img_q->front().trigger_message;
            timeStamp =  convertedImage->GetTimeStamp() * 1000;
            // Create a unique filename
            ostringstream filename;
            filename<<path_<<cam_names_[cam_no]<<"/"<<cam_names_[cam_no]
                    <<"_"<<id<<"_"<<todays_date_ << "_"<<std::setfill('0')
                    << std::setw(6) << imageCnt<<"_"<<timeStamp << ext_; 
            ml_grab_time_ = ros::Time::now().toSec() - t;
            t = ros::Time::now().toSec();
            if (SAVE_ ) {
                convertedImage->Save(filename.str().c_str());
                ROS_DEBUG_STREAM("Image saved at " << filename.str());
                ml_save_time_ = ros::Time::now().toSec() - t;
                t = ros::Time::now().toSec();

                boost::property_tree::ptree ptree;
                ptree.put("camera.block_name", trigger_message.block_name);
                ptree.put("camera.cam_no", cam_no);
                ptree.put("camera.image_number", trigger_message.image_number);
                ptree.put("camera.lat", trigger_message.lat);
                ptree.put("camera.lon", trigger_message.lon);
                ptree.put("camera.utm_x", trigger_message.utm_x);
                ptree.put("camera.utm_y", trigger_message.utm_y);
                ptree.put("camera.altitude", trigger_message.altitude);
                ptree.put("camera.heading", trigger_message.heading);

                std::ofstream file;
	            std::ostringstream oss;

	            boost::property_tree::write_json(oss, ptree);

                Exiv2::ExifData exif_data;
                exif_data["Exif.Image.Model"] = "Test 1";  
                exif_data["Exif.Image.ImageDescription"] = oss.str(); 
                Exiv2::Image::UniquePtr image_exif_file = Exiv2::ImageFactory::open(filename.str());
                image_exif_file->setExifData(exif_data);
				image_exif_file->writeMetadata();
                metadata_write_time_ = ros::Time::now().toSec() - t;
            }
            if (EXPORT_TO_ROS_){
                Mat mat_frame = convert_to_mat(convertedImage);
                ml_toMat_time_ = ros::Time::now().toSec() - t;
                t = ros::Time::now().toSec();
                std_msgs::Header img_msg_header;
                string frame_id_prefix;
                if (tf_prefix_.compare("") != 0)
                    frame_id_prefix = tf_prefix_ +"/";
                else frame_id_prefix="";

                img_msg_header.frame_id = frame_id_prefix + "cam_"+to_string(cam_no)+"_optical_frame";
                cam_info_msgs[cam_no]->header = img_msg_header;
                img_msgs[cam_no]=cv_bridge::CvImage(img_msg_header, "bgr8", mat_frame).toImageMsg();
                camera_image_pubs[cam_no].publish(img_msgs[cam_no],cam_info_msgs[cam_no]);
                
                msgs_and_srvs::GpsTaggedImageMsg gps_tagged_image;
                gps_tagged_image.image = *img_msgs[cam_no];
                
                gps_tagged_image.image_number = trigger_message.image_number;
                gps_tagged_image.block_name = trigger_message.block_name;
                gps_tagged_image.camera_number = cam_no;
                gps_tagged_image.lat = trigger_message.lat;
                gps_tagged_image.lon = trigger_message.lon;
                gps_tagged_image.utm_x = trigger_message.utm_x;
                gps_tagged_image.utm_y = trigger_message.utm_y;
                gps_tagged_image.altitude = trigger_message.altitude;
                gps_tagged_image.heading = trigger_message.heading;
                camera_image_gps_pubs[cam_no].publish(gps_tagged_image);
                ml_export_to_ROS_time_ = ros::Time::now().toSec() - t;
            }

            imageCnt++;
            double total_time = ml_grab_time_ + ml_save_time_ + metadata_write_time_+ ml_toMat_time_+export_to_ROS_time_;
            msgs_and_srvs::CollectionBenchmarkMsg benchmarkMsg;
            benchmarkMsg.totalTime = total_time*1000;
            benchmarkMsg.fps = 1/total_time;
            benchmarkMsg.grab = ml_grab_time_*1000;
            benchmarkMsg.save = ml_save_time_*1000;
            benchmarkMsg.writeMetadata = metadata_write_time_*1000;
            benchmarkMsg.convert = ml_toMat_time_*1000;
            benchmarkMsg.export2Ros = ml_export_to_ROS_time_*1000;
            benchmarkMsg.queueSize = (int)img_q->size();
            benchmark_pubs[cam_no].publish(benchmarkMsg);
            ROS_INFO_COND(TIME_BENCHMARK_,"total time (ms): %.1f \tFPS: %.1f",total_time*1000,1/total_time);
            ROS_INFO_COND(TIME_BENCHMARK_,"Times (ms):- grab: %.1f, save: %.1f, metadata: %.1f, toMat: %.1f, exp2ROS: %.1f",
                          ml_grab_time_*1000,ml_save_time_*1000,metadata_write_time_*1000,ml_toMat_time_*1000,ml_export_to_ROS_time_*1000);
            ROS_DEBUG_STREAM("Image Queue size for cam"<< cam_no <<" is ="<< img_q->size());
            
            // release the image before popping out to save memory
            convertedImage->Release();
            queue_mutex_.lock();
            img_q->pop();
            queue_mutex_.unlock();
        }
    }
    catch(const std::exception &e){
        ROS_FATAL_STREAM("Exception: "<<e.what());
    }
}

void acquisition::Capture::acquire_images_to_queue(vector<queue<Metadata>>*  img_qs) {    
    ROS_DEBUG("  Acquire Images to Queue Thread Initiated");
    start_acquisition();
    ROS_DEBUG("  Acquire Images to Queue Thread -> Acquisition Started");
    double t = ros::Time::now().toSec();
    double acquire_time = ros::Time::now().toSec();
    // Retrieve, convert, and save images for each camera
    try{
        while( ros::ok() ) {
            for (int i = 0; i < numCameras_; i++) {
                t = ros::Time::now().toSec();
                try {
                    //  grab_frame() is a blocking call. It waits for the next image acquired by the camera 
                    struct Metadata captured_image;
                    captured_image.image = cams[i].grab_frame();
                    captured_image.trigger_message = *nmea_trigger;
                    queue_mutex_.lock();
                    img_qs->at(i).push(captured_image);
                    queue_mutex_.unlock();
                    ROS_DEBUG_STREAM("Queue no. "<<i<<" size: "<<img_qs->at(i).size());
                    
                }
                catch (Spinnaker::Exception &e) {
                    ROS_ERROR_STREAM("  Exception in Acquire to queue thread" << "\nError: " << e.what());
                }
                if ( i == 0){
                    acquire_time = ros::Time::now().toSec() - t;
                    std_msgs::Float64 camerafpsMsg;
                    camerafpsMsg.data = 1/acquire_time;
                    camera_fps_pub.publish(camerafpsMsg);
                }
            }
        }
    }
    catch(const std::exception &e){
        ROS_FATAL_STREAM("Exception: "<<e.what());
    }
    return;
}

void acquisition::Capture::run_mt() {
    ROS_INFO("*** ACQUISITION MULTI-THREADED***");
    
    if (!CAM_DIRS_CREATED_)
        create_cam_directories();
    
    boost::thread_group threads;

    

    vector<std::queue<Metadata>> image_queue_vector;

    for (int i=0; i<numCameras_; i++) {
        std::queue<Metadata> img_ptr_queue;
        image_queue_vector.push_back(img_ptr_queue);
    }
    
    // start
    threads.create_thread(boost::bind(&Capture::acquire_images_to_queue, this, &image_queue_vector));

    // assign a new thread to write the nth image to disk acquired in a queue
    for (int i=0; i<numCameras_; i++)
        threads.create_thread(boost::bind(&Capture::write_queue_to_disk, this, &image_queue_vector.at(i), i));

    threads.join_all();
    ROS_DEBUG("All Threads Joined");
}

void acquisition::Capture::run() {
    if (MAX_RATE_SAVE_)
        run_mt();
    else
        run_soft_trig();
    ROS_DEBUG("Run completed");
}

std::string acquisition::Capture::todays_date()
{
    char out[9];
    std::time_t t=std::time(NULL);
    std::strftime(out, sizeof(out), "%Y%m%d", std::localtime(&t));
    std::string td(out);
    return td;
}


void acquisition::Capture::dynamicReconfigureCallback(spinnaker_sdk_camera_driver::spinnaker_camConfig &config, uint32_t level){
    
    ROS_INFO_STREAM("Dynamic Reconfigure: Level : " << level);
    if(level == 1 || level ==3){
        ROS_INFO_STREAM("Target grey value : " << config.target_grey_value);
        for (int i = numCameras_-1 ; i >=0 ; i--) {
            
            cams[i].setEnumValue("AutoExposureTargetGreyValueAuto", "Off");
            cams[i].setFloatValue("AutoExposureTargetGreyValue", config.target_grey_value);
        }
    }
    if (level == 2 || level ==3){
        ROS_INFO_STREAM("Exposure "<<config.exposure_time);
        if(config.exposure_time > 0){
            for (int i = numCameras_-1 ; i >=0 ; i--) {

                cams[i].setEnumValue("ExposureAuto", "Off");
                cams[i].setEnumValue("ExposureMode", "Timed");
                cams[i].setFloatValue("ExposureTime", config.exposure_time);
            }
        }
        else if(config.exposure_time ==0){
            for (int i = numCameras_-1 ; i >=0 ; i--) {
                cams[i].setEnumValue("ExposureAuto", "Continuous");
                cams[i].setEnumValue("ExposureMode", "Timed");
            }
        }
    }
}


void acquisition::Capture::assignSoftwareTriggerCallback(const msgs_and_srvs::ImageTriggerMsg::ConstPtr& msg){
    
    ROS_DEBUG_STREAM("Callback");
    nmea_trigger = *msg;
    trigger_capture_ = true;
    cams[MASTER_CAM_].trigger();
}


