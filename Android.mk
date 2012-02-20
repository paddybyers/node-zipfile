# Set up the following environment variables
# NODE_ROOT: location of the node root directory (for include files)
# ANODE_ROOT: location of the anode root directory (for the libjninode binary)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := _zipfile 

LOCAL_CFLAGS := \
	-D__POSIX__ \
	-DBUILDING_NODE_EXTENSION \
	-include sys/select.h

LOCAL_CPPFLAGS :=

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/include \
	$(NODE_ROOT)/src \
	$(NODE_ROOT)/deps/v8/include \
	$(NODE_ROOT)/deps/uv/include \
	$(NODE_ROOT)/deps/zlib/contrib/minizip

LOCAL_LDLIBS := \
	$(ANODE_ROOT)/libs/armeabi/libjninode.so \
	-llog \
	-lz

LOCAL_CPP_EXTENSION := .cc .cpp

LOCAL_SRC_FILES := \
	src/_zipfile.cc \
	src/node_zipfile.cpp

LOCAL_STATIC_LIBRARIES := \
	minizip

LOCAL_SHARED_LIBRARIES :=

# Do not edit this line.
include $(ANODE_ROOT)/sdk/addon/build-node-addon.mk

$(call import-module,deps/zlib/contrib/minizip)
