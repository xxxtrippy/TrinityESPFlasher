cmake_minimum_required(VERSION 3.16)

# Always build for the ESP32-S3 — no `idf.py set-target` needed.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(trinityweb360)
