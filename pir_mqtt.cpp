/**
 * @file pir_mqtt.cpp
 * @brief Processes PIR sensor data, streaming video and sending MQTT alerts on movement detection.
 * @author Eduard Polyakov <eduardpo@gmail.com>
 * @date 2025-07-06
 * @copyright (c) 2025 Eduard Polyakov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <iostream>
#include <fstream>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <getopt.h>
#include <mosquitto.h>
#include <gpiod.h>
#include <chrono>
#include <ctime>
#include <iomanip> // for std::put_time
#include <thread>
#include <gst/gst.h>
#include <opencv2/opencv.hpp>

#define MQTT_PAYLOAD_MOTION_DETECTED "motion_detected"
#define MQTT_PAYLOAD_MOTION_WAITING "motion_waiting"

static volatile bool running = true;

std::string mqtt_host = "10.100.102.4";
int mqtt_port = 1883;
std::string mqtt_topic = "pir/motion";
int gpio_line = 12;
std::string log_file = "/var/log/pir_mqtt.log";
std::string stream_ip = "192.168.1.100";
int stream_port = 5000;
int stream_cooldown_seconds = 30;
int stream_duration_ms = 30000;
int video_width = 640;
int video_height = 480;
int video_fps = 30;
bool save_snapshot = false;
std::string snapshot_path = "/var/log/motion_snapshot.jpg";


void log_event(const std::string& msg) {
    std::ofstream log(log_file, std::ios_base::app);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log << std::put_time(std::localtime(&now), "%c") << ": " << msg << std::endl;
}

void handle_signal(int sig) {
    log_event("Exiting on signal " + std::to_string(sig));
    running = false;
}

void mqtt_reconnect_loop(struct mosquitto *mosq) {
    while (mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        log_event("MQTT reconnect to host '" + mqtt_host + "' failed, retrying in 3s...");
        sleep(3);
    }
    log_event("Connected to MQTT broker at " + mqtt_host + ":" + std::to_string(mqtt_port));
}

void parse_config(const std::string &path) {
    std::ifstream file(path);
    std::string line;
    while (getline(file, line)) {
        if (line.find("gpio=") == 0) gpio_line = std::stoi(line.substr(5));
        else if (line.find("host=") == 0) mqtt_host = line.substr(5);
        else if (line.find("port=") == 0) mqtt_port = std::stoi(line.substr(5));
        else if (line.find("topic=") == 0) mqtt_topic = line.substr(6);
        else if (line.find("logfile=") == 0) log_file = line.substr(8);
        else if (line.find("stream_ip=") == 0) stream_ip = line.substr(10);
        else if (line.find("stream_port=") == 0) stream_port = std::stoi(line.substr(12));
        else if (line.find("streamcooldown=") == 0) stream_cooldown_seconds = std::stoi(line.substr(15));
        else if (line.find("streamduration=") == 0) stream_duration_ms = std::stoi(line.substr(15));
        else if (line.find("video_width=") == 0) video_width = std::stoi(line.substr(12));
        else if (line.find("video_height=") == 0) video_height = std::stoi(line.substr(13));
        else if (line.find("video_fps=") == 0) video_fps = std::stoi(line.substr(10));
        else if (line.find("save_snapshot=") == 0) save_snapshot = (line.substr(14) == "1");
        else if (line.find("snapshot_path=") == 0) snapshot_path = line.substr(14);
    }
}

void start_gstreamer_stream()
{
    GstElement *pipeline;
    GError *error = nullptr;

    gst_init(nullptr, nullptr);

    // Build GStreamer pipeline string
    std::string pipeline_str =
        "libcamerasrc ! "
        "video/x-raw,width=" + std::to_string(video_width) +
        ",height=" + std::to_string(video_height) +
        ",framerate=" + std::to_string(video_fps) + "/1,format=NV12 ! "
        "videoconvert ! "
        "x264enc tune=zerolatency ! "
        "rtph264pay mtu=1200 ! "
        "udpsink host=" + stream_ip +
        " port=" + std::to_string(stream_port);

    pipeline = gst_parse_launch(pipeline_str.c_str(), &error);

    if (!pipeline)
    {
        log_event("Failed to create GStreamer pipeline: " + std::string(error ? error->message : "Unknown error"));
        if (error) g_error_free(error);
        return;
    }

    log_event("Starting GStreamer video stream...");

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::milliseconds(stream_duration_ms));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    log_event("Stopped GStreamer video stream");
}

void capture_snapshot() {
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        log_event("Failed to open camera for snapshot");
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, video_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, video_height);
    cv::Mat frame;
    cap >> frame;
    if (!frame.empty()) {
        cv::imwrite(snapshot_path, frame);
        log_event("Snapshot saved to " + snapshot_path);
    } else {
        log_event("Failed to capture snapshot frame");
    }
    cap.release();
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_signal);

    std::string config_path;
    int opt;
    while ((opt = getopt(argc, argv, "g:h:p:t:c:l:")) != -1) {
        switch (opt) {
            case 'g': gpio_line = std::stoi(optarg); break;
            case 'h': mqtt_host = optarg; break;
            case 'p': mqtt_port = std::stoi(optarg); break;
            case 't': mqtt_topic = optarg; break;
            case 'c': config_path = optarg; break;
            case 'l': log_file = optarg; break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-c config] [-g gpio] [-h host] [-p port] [-t topic] [-l logfile]\n";
                return 1;
        }
    }

    if (!config_path.empty()) parse_config(config_path);

    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new(nullptr, true, nullptr);
    if (!mosq) {
        log_event("Failed to create mosquitto client");
        return 1;
    }

    int protocol_version = MQTT_PROTOCOL_V311;
    mosquitto_opts_set(mosq, MOSQ_OPT_PROTOCOL_VERSION, &protocol_version);
    mosquitto_reconnect_delay_set(mosq, 2, 10, false);

    mqtt_reconnect_loop(mosq);

    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    struct gpiod_line *line = gpiod_chip_get_line(chip, gpio_line);
    gpiod_line_request_both_edges_events(line, "pir_mqtt");

    struct gpiod_line_event event;
    int motion_count = 0;
    log_event("Monitoring started on GPIO" + std::to_string(gpio_line));

    auto last_stream = std::chrono::steady_clock::now() - std::chrono::seconds(stream_cooldown_seconds);

    while (running) {
        struct timespec timeout = {0, 100000000}; // 100 ms
        int ret = gpiod_line_event_wait(line, &timeout);
        if (ret == 1 && gpiod_line_event_read(line, &event) == 0 &&
            event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {

            motion_count++;
            log_event("Motion detected (#" + std::to_string(motion_count) + ")");
            int rc = mosquitto_publish(mosq, nullptr, mqtt_topic.c_str(),
                                       strlen(MQTT_PAYLOAD_MOTION_DETECTED), MQTT_PAYLOAD_MOTION_DETECTED, 0, false);
            if (rc != MOSQ_ERR_SUCCESS)
                log_event("MQTT publish failed: " + std::string(mosquitto_strerror(rc)));

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stream).count() >= stream_cooldown_seconds) {
                log_event("Starting video stream to " + stream_ip + ":" + std::to_string(stream_port));
                std::thread(start_gstreamer_stream).detach();
                if (save_snapshot) capture_snapshot();
                last_stream = now;
            }
            
        } else {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stream).count() >= stream_cooldown_seconds) {
                //log_event("Motion waiting 0");
                int rc = mosquitto_publish(mosq, nullptr, mqtt_topic.c_str(),
                                        strlen(MQTT_PAYLOAD_MOTION_WAITING), MQTT_PAYLOAD_MOTION_WAITING, 0, false);
                if (rc != MOSQ_ERR_SUCCESS)
                    log_event("MQTT publish failed: " + std::string(mosquitto_strerror(rc)));
            }
        }

        // Maintain MQTT connection
        mosquitto_loop_misc(mosq);
        mosquitto_loop_write(mosq, 1);
        mosquitto_loop_read(mosq, 1);
    }

    mosquitto_disconnect(mosq);
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    log_event("Shutdown complete. Total motions detected: " + std::to_string(motion_count));
    return 0;
}
