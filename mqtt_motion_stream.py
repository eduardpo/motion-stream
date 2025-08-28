#!/usr/bin/env python3

import subprocess
import signal
import sys
import paho.mqtt.client as mqtt

# Settings
MQTT_BROKER = "localhost"
MQTT_TOPIC = "pir/motion"
GST_COMMAND = [
    "gst-launch-1.0", "-v",
    "udpsrc", "port=5000",
    'caps=application/x-rtp,media=video,encoding-name=H264,payload=96',
    "!", "rtph264depay", "!", "avdec_h264", "!", "autovideosink"
]

gst_process = None

def start_gstreamer():
    global gst_process
    if gst_process is None:
        print("Starting GStreamer receiver...")
        gst_process = subprocess.Popen(GST_COMMAND)

def stop_gstreamer():
    global gst_process
    if gst_process:
        print("Stopping GStreamer receiver...")
        gst_process.terminate()
        try:
            gst_process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            gst_process.kill()
        gst_process = None

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT broker with code {rc}")
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    payload = msg.payload.decode()
    print(f"MQTT: {msg.topic} = {payload}")

    if payload == "motion_detected":
        start_gstreamer()
    elif payload == "motion_waiting":
        stop_gstreamer()

def cleanup(signum, frame):
    print("Signal received, cleaning up...")
    stop_gstreamer()
    sys.exit(0)

if __name__ == "__main__":
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_BROKER, 1883, 60)
    print("Waiting for MQTT messages...")
    client.loop_forever()

