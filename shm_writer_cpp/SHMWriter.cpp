// Compile: g++ -std=c++17 camera_supervisor.cpp -o camera_supervisor \
//          `pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0` \
//          -lzmq `pkg-config --cflags --libs opencv4` -pthread -lrt
//
// Example run: ./camera_supervisor
//
// Note: adjust pkg-config names on your platform if needed.
/* g++ -std=c++17 SHMWriter.cpp -o camera_supervisor     `pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0`     -lzmq `pkg-config --cflags --libs opencv4` -pthread -lrt*/

#include <iostream>
#include <thread>
#include <map>
#include <atomic>
#include <mutex>
#include <csignal>
#include <vector>
#include <memory>
#include <chrono>
#include <sstream>

#include <zmq.hpp>
#include <opencv2/opencv.hpp>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "SHMWriter_Ring_Buffer.hpp"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

std::atomic<bool> running{true};
std::mutex cam_mutex;

// CameraThread: captures from RTSP via GStreamer (decode -> appsink BGR), optionally resizes, writes to SHM.
class CameraThread {
public:
    CameraThread(int cam_id, const std::string& rtsp_url, int target_w = 0, int target_h = 0)
        : cam_id(cam_id), rtsp_url(rtsp_url), target_width(target_w), target_height(target_h),
          stop_flag(false), shm_initialized(false), resize_requested(false) {}

    ~CameraThread() { stop(); }

    void start() { thread_ = std::thread(&CameraThread::run, this); }
    void stop() {
        stop_flag = true;
        if(thread_.joinable()) thread_.join();
        // ensure SHM cleaned up for our writer (but do not unlink here)
        shm.cleanup();
    }

    void resizeTarget(int new_w, int new_h) {
        std::lock_guard<std::mutex> lock(resize_mutex);
        target_width = new_w;
        target_height = new_h;
        resize_requested = true;
    }

    // Unlink SHM (call when removing camera)
    void unlinkShm() {
        shm.unlinkSharedMemory();
    }

private:
    int cam_id;
    std::string rtsp_url;
    int target_width, target_height;
    std::atomic<bool> stop_flag;
    std::thread thread_;
    bool shm_initialized;
    std::mutex resize_mutex;
    bool resize_requested;

    SHMWriter_Ring_Buffer shm;  // Ring buffer with 30 slots (default)

    void run() {
        // Build pipeline string
        // rtspsrc -> decodebin -> videoconvert -> video/x-raw,format=BGR -> appsink
        // std::string pipeline_str =
        //     "rtspsrc location=" + rtsp_url + " latency=200 ! "
        //     "rtpjitterbuffer ! "
        //     "decodebin ! "
        //     "videoconvert ! "
        //     "video/x-raw,format=BGR ! "
        //     "appsink name=mysink emit-signals=false sync=false";
        std::string pipeline_str =
            "rtspsrc location=" + rtsp_url + " protocols=tcp latency=200 ! "
            "decodebin ! "
            "videoconvert ! "
            "video/x-raw,format=BGR ! "
            "appsink name=mysink emit-signals=false sync=false";


        GstElement* pipeline = nullptr;
        GstElement* appsink = nullptr;

        auto create_pipeline = [&]() -> bool {
            if(pipeline) {
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                pipeline = nullptr;
                appsink = nullptr;
            }
            GError* err = nullptr;
            pipeline = gst_parse_launch(pipeline_str.c_str(), &err);
            if(!pipeline) {
                std::cerr << "[Camera " << cam_id << "] Failed to create pipeline: " << (err ? err->message : "unknown") << "\n";
                if(err) g_error_free(err);
                return false;
            }
            appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
            if(!appsink) {
                std::cerr << "[Camera " << cam_id << "] appsink not found in pipeline\n";
                gst_object_unref(pipeline);
                pipeline = nullptr;
                return false;
            }
            // Configure appsink
            gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 2);
            gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            return true;
        };

        if(!create_pipeline()) {
            std::cerr << "[Camera " << cam_id << "] initial pipeline creation failed. Exiting thread.\n";
            if(pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); }
            return;
        }

        while(!stop_flag) {
            // Pull sample with timeout to allow shutdown checking
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 5 * GST_MSECOND);
            if(!sample) {
                // no sample received -> maybe temporary network issue
                // Check stop flag and continue/retry (or attempt pipeline restart on repeated failures).
                if(stop_flag) break;
                // simple continue; could implement better reconnect/backoff logic
                continue;
            }

            GstCaps* caps = gst_sample_get_caps(sample);
            if(!caps) { gst_sample_unref(sample); continue; }
            GstStructure* structure = gst_caps_get_structure(caps, 0);
            int orig_width = 0, orig_height = 0;
            gst_structure_get_int(structure, "width", &orig_width);
            gst_structure_get_int(structure, "height", &orig_height);

            // Initialize SHM once with original frame size
            if(!shm_initialized) {
                if(!shm.init(cam_id, orig_width, orig_height)) {
                    std::cerr << "[Camera " << cam_id << "] SHM init failed\n";
                    gst_sample_unref(sample);
                    break;
                }
                shm_initialized = true;
                std::cout << "[Camera " << cam_id << "] SHM initialized " << orig_width << "x" << orig_height << "\n";
            }

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if(gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                // Wrap data into cv::Mat (no copy)
                cv::Mat frame(orig_height, orig_width, CV_8UC3, (void*)map.data);

                // Possibly resize
                int write_w = orig_width, write_h = orig_height;
                cv::Mat out_frame = frame;
                {
                    std::lock_guard<std::mutex> lock(resize_mutex);
                    if(target_width > 0 && target_height > 0 && resize_requested) {
                        // only resize if smaller or equal to original (you may change this policy)
                        if(target_width <= orig_width && target_height <= orig_height) {
                            cv::resize(frame, out_frame, cv::Size(target_width, target_height));
                            write_w = target_width;
                            write_h = target_height;
                        } else {
                            std::cerr << "[Camera " << cam_id << "] Requested resize exceeds original frame. Ignoring.\n";
                        }
                        resize_requested = false;
                    }
                }

                // Write to SHM (out_frame is contiguous unless ROI; ensure contiguity)
                if(!out_frame.isContinuous()) out_frame = out_frame.clone();
                shm.writeFrame(out_frame.data, write_w, write_h);

                gst_buffer_unmap(buffer, &map);
            } else {
                std::cerr << "[Camera " << cam_id << "] gst_buffer_map failed\n";
            }
            gst_sample_unref(sample);
        }

        // Cleanup pipeline on exit
        if(pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
            appsink = nullptr;
        }
    }
};

// Supervisor: listens on ZMQ REP socket for commands:
// add:<id>:<rtsp_url>:<w>:<h>
// remove:<id>
// resize:<id>:<w>:<h>
// stop
class Supervisor {
public:
    Supervisor() : context(1), socket(context, ZMQ_REP) {
        // set socket receive timeout so recv won't block forever and thread can exit on running=false
        int rcv_timeout = 500; // ms
        socket.set(zmq::sockopt::rcvtimeo, 1000);

    }

    ~Supervisor() { stop(); }

    void start() {
        try {
            socket.bind("tcp://0.0.0.0:5555");
        } catch(const zmq::error_t& e) {
            std::cerr << "[Supervisor] ZMQ bind failed: " << e.what() << "\n";
            throw;
        }
        zmqListenerThread = std::thread(&Supervisor::listenIPC, this);
    }

    void stop() {
        // stop listening thread
        if(zmqListenerThread.joinable()) {
            running = false;
            zmqListenerThread.join();
        }

        // stop cameras
        std::lock_guard<std::mutex> lock(cam_mutex);
        for(auto &kv : camera_threads) {
            if(kv.second) {
                kv.second->stop();
                // Unlink SHM for this camera
                kv.second->unlinkShm();
            }
        }
        camera_threads.clear();
    }

private:
    std::map<int, std::shared_ptr<CameraThread>> camera_threads;
    std::mutex cam_mutex;

    zmq::context_t context;
    zmq::socket_t socket;
    std::thread zmqListenerThread;

   void listenIPC() {

        while (running) {
            zmq::message_t request;

            if (!socket.recv(request, zmq::recv_flags::none).has_value())
                continue;

            std::string msg(static_cast<char*>(request.data()), request.size());
            json cmd;
            json reply;

            // ---------- Parse JSON ----------
            try {
                cmd = json::parse(msg);
            }
            catch (...) {
                reply["status"] = "error";
                reply["msg"] = "invalid-json";

                socket.send(zmq::buffer(reply.dump()), zmq::send_flags::none);
                continue;   // ✅ one reply sent
            }

            // ---------- Handle Command ----------
            std::string type = cmd.value("cmd", "");

            if (type == "START_CAMERA") {
                addCamera(
                    cmd["camera_id"],
                    cmd["rtsp"],
                    cmd["desired_width"],
                    cmd["desired_height"]
                );

                reply["status"] = "ok";
            }
            else if (type == "STOP_CAMERA") {
                removeCamera(cmd["camera_id"]);

                reply["status"] = "ok";
            }
            else {
                reply["status"] = "error";
                reply["msg"] = "unknown-command";
            }

            // ---------- Send ONE reply ----------
            socket.send(zmq::buffer(reply.dump()), zmq::send_flags::none);
        }
    }


    void addCamera(int cam_id, const std::string& rtsp_url, int w, int h) {
        std::lock_guard<std::mutex> lock(cam_mutex);
        if(camera_threads.find(cam_id) != camera_threads.end()) {
            std::cout << "[Supervisor] camera " << cam_id << " already exists\n";
            return;
        }
        auto cam = std::make_shared<CameraThread>(cam_id, rtsp_url, w, h);
        camera_threads[cam_id] = cam;
        cam->start();
        std::cout << "[Supervisor] Camera " << cam_id << " added\n";
    }

    void removeCamera(int cam_id) {
        std::lock_guard<std::mutex> lock(cam_mutex);
        auto it = camera_threads.find(cam_id);
        if(it == camera_threads.end()) {
            std::cout << "[Supervisor] camera " << cam_id << " not found\n";
            return;
        }
        it->second->stop();
        it->second->unlinkShm(); // unlink SHM explicitly when removing
        camera_threads.erase(it);
        std::cout << "[Supervisor] Camera " << cam_id << " removed\n";
    }

    void resizeCamera(int cam_id, int w, int h) {
        std::lock_guard<std::mutex> lock(cam_mutex);
        auto it = camera_threads.find(cam_id);
        if(it == camera_threads.end()) {
            std::cout << "[Supervisor] camera " << cam_id << " not found\n";
            return;
        }
        it->second->resizeTarget(w, h);
        std::cout << "[Supervisor] resize requested for " << cam_id << " -> " << w << "x" << h << "\n";
    }

    // simple split helper
    std::vector<std::string> split(const std::string& str, char delim) {
        std::vector<std::string> res;
        size_t start = 0, end;
        while((end = str.find(delim, start)) != std::string::npos) {
            res.push_back(str.substr(start, end-start));
            start = end+1;
        }
        res.push_back(str.substr(start));
        return res;
    }
};

void signalHandler(int signum) {
    running = false;
}

int main(int argc, char** argv) {
    // call gst_init once
    gst_init(nullptr, nullptr);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        Supervisor supervisor;
        supervisor.start();

        std::cout << "Supervisor started. Listening on tcp://*:5555\n";
        // main thread sleeps while running; Supervisor handles ZMQ & cameras
        while(running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "Shutting down supervisor...\n";
        supervisor.stop();
    } catch(const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Exited cleanly.\n";
    return 0;
}
