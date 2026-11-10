#include "android.h"

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <jni.h>

#include <map>
#include <mutex>
#endif

namespace launcher {

std::vector<std::string> &play_request() {
    static std::vector<std::string> request;
    return request;
}

#ifdef __ANDROID__

namespace {

std::mutex lock;
std::map<std::string, std::string> path_of_uri, uri_of_path;
bool asked = false;

// the activity's askAllFiles: Android's screen where the player lets the
// app read the phone's files
void ask_for_files() {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass type = env->GetObjectClass(activity);
    if (jmethodID method = env->GetStaticMethodID(type, "askAllFiles", "()V")) env->CallStaticVoidMethod(type, method);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

// the activity's openDocument (android/.../PortActivity.java): the document
// opened for reading, its permission kept for the next runs; -1 when not
int open_document(const std::string &uri) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return -1;
    int fd = -1;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetStaticMethodID(type, "openDocument", "(Ljava/lang/String;)I");
    if (method) {
        jstring text = env->NewStringUTF(uri.c_str());
        fd = env->CallStaticIntMethod(type, method, text);
        env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        fd = -1;
    }
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return fd;
}

} // namespace

std::string local_path(const std::string &path) {
    if (path.rfind("content://", 0) != 0) return path;
    std::lock_guard<std::mutex> hold(lock);
    if (auto known = path_of_uri.find(path); known != path_of_uri.end()) return known->second;
    int fd = open_document(path);
    if (fd < 0) {
        SDL_Log("document %s: Android would not open it", path.c_str());
        return path;
    }
    // left open to the programs this one starts (3dsrecomp reads the game)
    fcntl(fd, F_SETFD, 0);
    std::string local = "/proc/self/fd/" + std::to_string(fd);
    // it is read by that name, opened again, which the phone's shared
    // storage refuses an app that may not read its files, whatever the
    // picker granted: the player is asked to let it
    int again = open(local.c_str(), O_RDONLY | O_CLOEXEC);
    if (again < 0) {
        SDL_Log("document %s: descriptor %d not opened again, %s", path.c_str(), fd, strerror(errno));
        close(fd);
        asked = true;
        ask_for_files();
        return path;
    }
    close(again);
    path_of_uri[path] = local;
    uri_of_path[local] = path;
    return local;
}

std::string kept_path(const std::string &path) {
    std::lock_guard<std::mutex> hold(lock);
    auto known = uri_of_path.find(path);
    return known == uri_of_path.end() ? path : known->second;
}

bool asked_for_files() {
    std::lock_guard<std::mutex> hold(lock);
    bool was = asked;
    asked = false;
    return was;
}

#else

bool asked_for_files() { return false; }

std::string local_path(const std::string &path) { return path; }
std::string kept_path(const std::string &path) { return path; }

#endif

} // namespace launcher
