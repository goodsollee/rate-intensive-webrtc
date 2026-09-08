#include "sdk/android/src/jni/finesse/frame_delay_sink.h"

#include <jni.h>

#include "api/media_stream_interface.h"

namespace {
finesse::FrameDelaySink* g_sink = nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_org_appspot_apprtc_NativeFrameDelay_nativeAttach(JNIEnv*,
                                                      jclass,
                                                      jlong native_track) {
  delete g_sink;
  g_sink = nullptr;
  auto* track =
      reinterpret_cast<webrtc::VideoTrackInterface*>(native_track);
  if (!track)
    return;
  g_sink = new finesse::FrameDelaySink(track);
}

extern "C" JNIEXPORT void JNICALL
Java_org_appspot_apprtc_NativeFrameDelay_nativeDetach(JNIEnv*, jclass) {
  delete g_sink;
  g_sink = nullptr;
}
