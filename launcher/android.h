// The launcher on Android, where frontend/android_main.cpp runs it and then
// the player in one program (libmain.so): what the system does its own way.
// Elsewhere each of these leaves things as they are.
#ifndef LAUNCHER_ANDROID_H
#define LAUNCHER_ANDROID_H

#include <string>
#include <vector>

namespace launcher {

// a file picked through Android's picker (content://...) opened, as a path
// this program and the ones it starts read it by (/proc/self/fd/N, the
// descriptor left to them); any other path as it is
std::string local_path(const std::string &path);
// whether local_path had to ask the player, in Android's settings, to let
// the app read the phone's files (a picked file it could not read) since
// the last time this was asked
bool asked_for_files();
// back: the content:// a path of local_path's stands for, to keep in the
// settings (the descriptor is this run's); any other path as it is
std::string kept_path(const std::string &path);

// the player's arguments when the launcher closed to play (Android's player
// is in the same program); empty when it closed for good
std::vector<std::string> &play_request();

} // namespace launcher

#endif
