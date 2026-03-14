LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := media_test_api_server.cpp

LOCAL_SHARED_LIBRARIES := \
    libbase \
    liblog

LOCAL_CFLAGS := -Wall -Werror

LOCAL_MODULE := media_test_api_server
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)
