#include "Watch.h"
#include "Board.h"
#include "Audio.h"
#include "SetLog.h"
#include "ViewerStream.h"
#include "WorkoutTimer.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include <cstdarg>
#include <cstring>

namespace {
using namespace motion;
enum class Page { Startup, Home, Settings, Help, ResetPresets, Calibrate, Apply, Poses, Angles, Axis, Tolerances, Name,
                  Notes, NoteEdit, Timer, Loss, PoseCountdown, Capture, Result,
                  SetCountdown, Live, Summary, Fault };
enum class Action { Home, Settings, Help, VolumeDown, VolumeUp, Wireless, ResetPresets, ConfirmReset, Calibrate, Apply, Slot, PresetPage,
                    OpenCalibration, Poses, Angles, Axis, SaveAxis, Tolerances, MainDegrees, OffDegrees,
                    Name, Type, Shift, Clear, Erase, SaveName, Notes, NoteEdit, SaveNotes, NotePage,
                    Timer, TimerAdjust, TimerStart, TimerStop, Loss, Pose,
                    Start, Cancel, Stop, Older, Newer };
struct Button { int x, y, w, h; Action action; int value; };
const char *const names[] = {"1. Start range of motion", "2. Start concentric",
                             "3. End range of motion", "4. Start eccentric"};
const char *const keys[] = {"bottom", "concentric", "top", "eccentric"};
constexpr int presetCount = 8, presetsPerPage = 4;
constexpr uint8_t lossOptions[] = {20, 40, 60, 80, 100};
constexpr uint8_t mainOptions[] = {5, 10, 15}, offOptions[] = {10, 15, 20};
struct Preset {
  char name[setlog::exerciseNameCapacity];
  Vec poses[PoseCount];
  bool saved[PoseCount];
  TrackingSettings tracking;
};
constexpr uint32_t countdownMs = 5000, captureMs = 3000, maxGapMs = 55;
constexpr uint32_t menuPulseMs = 35;
constexpr uint16_t bg = 0x0863, card = 0x19CA, accent = 0x77D8, ink = 0xFFFF;
constexpr uint16_t muted = 0xA5DA, red = 0xB148, blue = 0x1A96;
Arduino_ESP32QSPI bus(12, 11, 4, 5, 6, 7);
#ifdef CO5300_TFTWIDTH
// Waveshare's bundled driver.
Arduino_CO5300 panel(&bus, 8, 0, 410, 502, 22, 0, 0, 0);
#else
// Library Manager's 1.6.0 constructor includes the ips argument before width.
Arduino_CO5300 panel(&bus, 8, 0, false, 410, 502, 22, 0, 0, 0);
#endif
Arduino_Canvas canvas(410, 502, &panel);
Preferences nvm;
Page page = Page::Startup;
Button buttons[44];
int buttonCount = 0, selected = 0, countdown = 0, battery = -1, sampleHz = 0;
int slot = 0, presetPage = 0;
bool redraw = true, haveTilt = false, screenReady = false, boardReady = false;
bool pressedBefore = false, pulsing = false;
bool logStarted = false, summarySaving = false;
uint32_t lastRender, lastTouch, lastBattery, pulseStart, pulseLength;
uint32_t countdownStart, activeSince, lastSample, rateStart, rateCount;
uint32_t setElapsed, startupReadySince;
int startupProgress = -1;
const char *notice = "", *detail = "";
const char *volumeNotice = "";
const char *resetNotice = "", *axisNotice = "";
uint8_t volume = audio::defaultVolume;
Preset presets[presetCount]{};
char draftName[setlog::exerciseNameCapacity]{};
char notes[1025]{}, draftNotes[sizeof(notes)]{};
constexpr size_t noteColumns = 29, noteRows = 10;
int notePage = 0;
WorkoutTimer timer;
viewerstream::Status displayedWireless = viewerstream::Status::Unavailable;
bool uppercase = true, nameSaveFailed = false;
uint8_t velocityLoss = 0;
Vec latest{};
Capture capture;
RepSet session;
std::vector<uint32_t> times;
size_t review = 0;

bool fresh(uint32_t now) { return haveTilt && now - lastSample <= maxGapMs; }
bool setPage() { return page == Page::Live || page == Page::SetCountdown; }
bool posePage() { return page == Page::PoseCountdown || page == Page::Capture; }
void show(Page next) {
  page = next;
  buttonCount = 0;
  redraw = true;
}
void updateStartup(uint32_t now) {
  startupProgress = board::imuCalibrationProgress();
  if (!fresh(now)) {
    startupReadySince = 0;
  } else if (!startupReadySince) {
    startupReadySince = now;
    redraw = true;
  } else if (now - startupReadySince >= 900) {
    show(Page::Home);
  }
}
Preset &exercise() { return presets[slot]; }
void poseKey(char (&key)[16], int preset, int pose) {
  // Keep the original calibration in the first slot.
  if (preset == 0) snprintf(key, sizeof(key), "%s", keys[pose]);
  else snprintf(key, sizeof(key), "p%d_%d", preset, pose);
}
void nameKey(char (&key)[16], int preset) {
  snprintf(key, sizeof(key), "name%d", preset);
}
void axisKey(char (&key)[16], int preset) {
  snprintf(key, sizeof(key), "axis%d", preset);
}
void toleranceKey(char (&key)[16], int preset) {
  snprintf(key, sizeof(key), "tolerance%d", preset);
}
void loadPreset(int index) {
  Preset preset{};
  char key[16];
  nameKey(key, index);
  if (!nvm.isKey(key) || !nvm.getString(key, preset.name, sizeof(preset.name)) || !preset.name[0])
    snprintf(preset.name, sizeof(preset.name), "Exercise %d", index + 1);
  axisKey(key, index);
  if (nvm.isKey(key)) {
    uint8_t axis = nvm.getUChar(key);
    if (axis <= uint8_t(TrackingAxis::Roll)) preset.tracking.axis = TrackingAxis(axis);
  }
  toleranceKey(key, index);
  if (nvm.isKey(key)) {
    uint16_t saved = nvm.getUShort(key);
    for (uint8_t value : mainOptions) if (value == (saved >> 8)) preset.tracking.mainDegrees = value;
    for (uint8_t value : offOptions) if (value == (saved & 0xff)) preset.tracking.offDegrees = value;
  }
  for (int i = 0; i < PoseCount; ++i) {
    poseKey(key, index, i);
    Vec pose;
    if (nvm.isKey(key) && nvm.getBytes(key, &pose, sizeof(pose)) == sizeof(pose) &&
        std::fabs(pose.norm() - 1.0f) < 0.002f) {
      preset.poses[i] = pose.unit();
      preset.saved[i] = true;
    }
  }
  presets[index] = preset;
}
bool removeSavedKey(const char *key) {
  return !nvm.isKey(key) || nvm.remove(key);
}
void resetPresets() {
  bool success = true;
  for (int index = 0; index < presetCount; ++index) {
    char key[16];
    nameKey(key, index);
    success = removeSavedKey(key) && success;
    axisKey(key, index);
    success = removeSavedKey(key) && success;
    toleranceKey(key, index);
    success = removeSavedKey(key) && success;
    for (int pose = 0; pose < PoseCount; ++pose) {
      poseKey(key, index, pose);
      success = removeSavedKey(key) && success;
    }
    loadPreset(index);
  }
  presetPage = 0;
  resetNotice = success ? "Exercise presets reset" : "Reset incomplete. Try again.";
}
void pulse(uint32_t duration) {
  pulseStart = millis();
  pulseLength = duration;
  pulsing = true;
  if (!timer.alarming(pulseStart)) board::motor(true);
}
void silence() {
  pulsing = false;
  board::motor(false);
}
void endSet(const char *why, uint32_t now) {
  setElapsed = page == Page::Live && int32_t(now - activeSince) >= 0 ? now - activeSince : 0;
  if (logStarted) setlog::finish(setElapsed, why, session);
  silence();
  audio::play(audio::Cue::Silence);
  notice = why;
  review = times.empty() ? 0 : times.size() - 1;
  show(Page::Summary);
}
void captureResult(const char *title, const char *message) {
  notice = title;
  detail = message;
  show(Page::Result);
}
void sensorGap() {
  if (setPage()) endSet("IMU signal lost", lastSample);
  else if (posePage()) {
    silence();
    captureResult("Capture interrupted", "IMU signal lost. Try again.");
  }
}
const char *calibrationIssue(const Preset &preset) {
  for (bool saved : preset.saved) if (!saved) return "Save all four poses first.";
  const auto &poses = preset.poses;
  if (poseDistance(poses[Bottom], poses[Concentric], preset.tracking.axis) <= minimumPoseSeparation)
    return "Bottom / concentric overlap.";
  if (poseDistance(poses[Top], poses[Concentric], preset.tracking.axis) <= minimumPoseSeparation)
    return "Top / concentric overlap.";
  if (poseDistance(poses[Bottom], poses[Top], preset.tracking.axis) <= minimumPoseSeparation)
    return "Bottom / top overlap.";
  return nullptr;
}
void finishCapture() {
  if (capture.count < 270 || !capture.stable) {
    captureResult("Try this pose again", "Hold still for all 3 seconds.");
    return;
  }
  Vec average = capture.sum.unit();
  char key[16];
  poseKey(key, slot, selected);
  if (nvm.putBytes(key, &average, sizeof(average)) != sizeof(average)) {
    captureResult("Could not save pose", "Check storage and try again.");
    return;
  }
  exercise().poses[selected] = average;
  exercise().saved[selected] = true;
  captureResult("Pose saved", names[selected]);
  pulse(650);
}
void startCountdown(Page next, uint32_t now) {
  silence();
  countdownStart = now;
  countdown = 5;
  show(next);
  pulse(85);
}
void tickCountdown(uint32_t now) {
  if (page != Page::PoseCountdown && page != Page::SetCountdown) return;
  uint32_t age = now - countdownStart;
  if (age >= countdownMs) {
    activeSince = countdownStart + countdownMs;
    if (page == Page::SetCountdown) {
      if (fresh(now)) session.prime(latest);
      logStarted = setlog::start(exercise().name, session.velocityLoss, exercise().tracking, session.tolerances());
      audio::play(audio::Cue::LetsGo);
      show(Page::Live);
    }
    else { capture = {}; show(Page::Capture); }
    return;
  }
  int remaining = 5 - age / 1000;
  if (remaining != countdown) {
    countdown = remaining;
    pulse(85);
    redraw = true;
  }
}
void acceptSample(const board::Sample &s) {
  if (haveTilt && s.ms - lastSample > maxGapMs) sensorGap();
  latest = s.tilt;
  haveTilt = true;
  lastSample = s.ms;
  ++rateCount;
  if (int32_t(s.ms - activeSince) < 0) {
    if (page == Page::Live) session.prime(s.tilt);
    return;
  }
  if (page == Page::Capture) {
    if (s.ms - activeSince < captureMs) capture.add(s.tilt);
    else finishCapture();
  } else if (page == Page::Live) {
    uint32_t before = session.count;
    session.sample(s.tilt, s.ms, exercise().poses);
    bool completed = session.count != before;
    if (logStarted)
      setlog::append(s, s.ms - activeSince, session.phase, session.count, completed ? session.last : 0);
    if (completed) {
      times.push_back(session.last);
      Serial.printf("Rep %lu: %lu ms (first %lu ms)\n",
                    (unsigned long)session.count, (unsigned long)session.last,
                    (unsigned long)session.first);
      redraw = true;
    }
    if (session.count >= maxSetReps) {
      endSet("20-rep limit reached", s.ms);
      pulse(650);
    } else if (session.slowRep) {
      endSet("Velocity loss cutoff reached", s.ms);
      pulse(650);
    } else if (session.rested) {
      endSet("Rested at bottom", s.ms);
    }
    if (completed) audio::playRep(session.count);
  }
}

void label(int x, int y, int size, uint16_t color, const char *format, ...) {
  char buf[80];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  canvas.setTextColor(color);
  canvas.setTextSize(size);
  canvas.setCursor(x,y);
  canvas.print(buf);
}
void button(Action a, const char *title, int y, uint16_t color = card,
            int x = 22, int w = 366, int h = 56, int size = 3, int value = 0) {
  buttons[buttonCount++] = {x, y, w, h, a, value};
  canvas.fillRoundRect(x, y, w, h, 12, color);
  label(x+16, y+(h-size*8)/2, size, ink, "%s", title);
}
void orientation(int y, bool compact = false) {
  if (fresh(millis())) {
    if (compact) label(26,y,2,muted,"%+.1f  %+.1f  %d", roll(latest), pitch(latest), sampleHz);
    else label(26,y,2,muted,"Roll %+.1f   Pitch %+.1f", roll(latest), pitch(latest));
  } else {
    int progress = board::imuCalibrationProgress();
    if (progress < 0) label(26,y,2,muted,"Waiting for IMU data");
    else label(26,y,2,muted,"Waiting for stable tilt");
  }
}
void drawStartup() {
  bool ready = startupReadySince != 0;
  int percent = ready ? 100 : constrain(startupProgress * 100 / int(TiltFilter::startupSamples), 0, 99);
  label(26,90,3,ink,"%s",ready ? "Gyro ready" : "Hold the watch still");
  if (ready) {
    label(26,151,2,muted,"You can move the watch now.");
  } else {
    label(26,151,2,muted,"Set it down on a table");
    label(26,178,2,muted,"or rest your wrist on one.");
    label(26,214,2,muted,"Keep still for about a second.");
  }
  canvas.fillRoundRect(22,262,366,114,12,card);
  label(44,281,4,accent,"%d%%",percent);
  constexpr int barWidth = 322;
  canvas.fillRect(44,335,barWidth,16,bg);
  if (percent) canvas.fillRect(44,335,barWidth * percent / 100,16,accent);
  if (!ready) {
    const char *status = startupProgress < 0 ? "Waiting for sensor readings" :
                         percent == 0 ? "Waiting for steady readings" : "Calibrating gyro";
    label(26,400,2,ink,"%s",status);
    label(26,434,2,muted,"Movement restarts the bar.");
  }
}
void drawPresets(bool calibrating) {
  label(24,77,3,ink,"%s",calibrating ? "Before you track" : "Start tracking");
  label(26,119,2,muted,"Exercise presets  %d / 2",presetPage + 1);
  for (int i = 0; i < presetsPerPage; ++i) {
    int index = presetPage * presetsPerPage + i;
    const Preset &preset = presets[index];
    int y = 149 + i * 60;
    const char *issue = calibrationIssue(preset);
    button(Action::Slot, preset.name, y, calibrating || issue ? card : blue,
           22, 366, 54, 2, index);
    if (!issue) label(361,y+20,1,accent,"OK");
  }
  button(Action::Home,"Back",417,card,22,174);
  button(Action::PresetPage,presetPage ? "Previous" : "Next",417,card,210,178);
}
const char *axisName(TrackingAxis axis) {
  switch (axis) {
    case TrackingAxis::Pitch: return "Pitch";
    case TrackingAxis::Roll: return "Roll";
    default: return "Both";
  }
}
void drawTiltPlane(int x, int y, TrackingAxis axis) {
  struct Point { int x, y; };
  const Point base[] = {{x-56,y+16}, {x+28,y+16}, {x+56,y-8}, {x-28,y-8}};
  Point tilted[] = {base[0], base[1], base[2], base[3]};
  int raised = axis == TrackingAxis::Pitch ? 3 : 1;
  tilted[raised].y -= 30;
  tilted[2].y -= 30;
  for (int i = 0; i < 4; ++i) {
    int next = (i + 1) % 4;
    canvas.drawLine(base[i].x,base[i].y,base[next].x,base[next].y,muted);
  }
  canvas.fillTriangle(tilted[0].x,tilted[0].y,tilted[1].x,tilted[1].y,tilted[2].x,tilted[2].y,blue);
  canvas.fillTriangle(tilted[0].x,tilted[0].y,tilted[2].x,tilted[2].y,tilted[3].x,tilted[3].y,blue);
  for (int i = 0; i < 4; ++i) {
    int next = (i + 1) % 4;
    canvas.drawLine(tilted[i].x,tilted[i].y,tilted[next].x,tilted[next].y,ink);
  }
  int hinge = axis == TrackingAxis::Pitch ? 1 : 3;
  canvas.drawLine(base[0].x,base[0].y,base[hinge].x,base[hinge].y,accent);
  int arrowX = (base[raised].x + base[2].x) / 2;
  int arrowY = (tilted[raised].y + tilted[2].y) / 2;
  canvas.drawFastVLine(arrowX,arrowY,31,accent);
  canvas.drawLine(arrowX-4,arrowY+5,arrowX,arrowY,accent);
  canvas.drawLine(arrowX+4,arrowY+5,arrowX,arrowY,accent);
}
void drawAxisMenu() {
  TrackingAxis axis = exercise().tracking.axis;
  label(24,77,3,ink,"Tracking axis");
  label(26,120,2,muted,"Which tilt matters most?");
  canvas.fillRoundRect(22,151,174,108,12,card);
  canvas.fillRoundRect(210,151,178,108,12,card);
  drawTiltPlane(109,198,TrackingAxis::Pitch);
  drawTiltPlane(299,198,TrackingAxis::Roll);
  label(31,230,2,ink,"Front to back");
  label(226,230,2,ink,"Side to side");
  button(Action::SaveAxis,"Pitch",267,axis == TrackingAxis::Pitch ? blue : card,
         22,174,48,2,int(TrackingAxis::Pitch));
  button(Action::SaveAxis,"Roll",267,axis == TrackingAxis::Roll ? blue : card,
         210,178,48,2,int(TrackingAxis::Roll));
  button(Action::SaveAxis,"Both axes (standard)",323,axis == TrackingAxis::Both ? blue : card,
         22,366,44,2,int(TrackingAxis::Both));
  if (*axisNotice) label(26,387,2,muted,"%s",axisNotice);
  else if (axis != TrackingAxis::Both) {
    const auto &tracking = exercise().tracking;
    label(26,387,2,muted,"%s +/- %u  %s +/- %u deg",axisName(axis),unsigned(tracking.mainDegrees),
          axisName(otherAxis(axis)),unsigned(tracking.offDegrees));
  }
  else label(26,387,2,muted,"Uses full wrist tilt");
  button(Action::Poses,"Back",417,card,22,174);
  button(Action::Tolerances,"Next",417,blue,210,178);
}
void drawToleranceMenu() {
  const TrackingSettings &tracking = exercise().tracking;
  label(24,77,3,ink,"Angle tolerances");
  if (tracking.axis == TrackingAxis::Both) {
    label(26,146,2,muted,"Both axes uses full wrist tilt.");
    label(26,180,2,muted,"Choose Pitch or Roll to set");
    label(26,206,2,muted,"main and off-axis tolerances.");
    button(Action::Axis,"Back",417);
    return;
  }
  label(26,127,2,muted,"Main axis  %s",axisName(tracking.axis));
  label(26,234,2,muted,"Off axis   %s",axisName(otherAxis(tracking.axis)));
  for (int i = 0; i < 3; ++i) {
    char title[12];
    int x = 22 + i * 124;
    snprintf(title,sizeof(title),"%u deg",unsigned(mainOptions[i]));
    button(Action::MainDegrees,title,158,mainOptions[i] == tracking.mainDegrees ? blue : card,
           x,118,55,2,mainOptions[i]);
    snprintf(title,sizeof(title),"%u deg",unsigned(offOptions[i]));
    button(Action::OffDegrees,title,265,offOptions[i] == tracking.offDegrees ? blue : card,
           x,118,55,2,offOptions[i]);
  }
  if (*axisNotice) label(26,352,2,muted,"%s",axisNotice);
  else label(26,352,2,muted,"Close poses can narrow gates.");
  button(Action::Axis,"Back",417,card,22,174);
  button(Action::Poses,"Done",417,blue,210,178);
}
const char *noteLine(const char *start, char (&line)[noteColumns + 1]) {
  size_t length = 0;
  while (length < noteColumns && start[length] && start[length] != '\n') ++length;
  memcpy(line, start, length);
  line[length] = '\0';
  return start + length + (start[length] == '\n');
}
void drawNotes() {
  label(24,77,4,ink,"Notes");
  char line[noteColumns + 1];
  const char *next = notes;
  for (int i = 0; i < notePage * int(noteRows) && *next; ++i) next = noteLine(next, line);
  if (!*notes) label(26,149,2,muted,"Add notes for your sets.");
  for (size_t row = 0; row < noteRows && *next; ++row) {
    next = noteLine(next, line);
    label(26,128+int(row)*22,2,ink,"%s",line);
  }
  if (notePage) button(Action::NotePage,"Previous",356,card,22,174,48,2,-1);
  if (*next) button(Action::NotePage,"Next",356,card,210,178,48,2,1);
  button(Action::Home,"Back",417,card,22,174);
  button(Action::NoteEdit,"Edit",417,blue,210,178);
}
void drawKeyboard(bool forNotes) {
  label(24,77,3,ink,"%s",forNotes ? "Edit notes" : "Exercise name");
  if (forNotes) {
    char previous[noteColumns + 1]{}, line[noteColumns + 1]{};
    const char *next = draftNotes;
    while (*next) {
      strcpy(previous, line);
      next = noteLine(next, line);
    }
    size_t length = strlen(draftNotes);
    if (length && (draftNotes[length - 1] == '\n' || strlen(line) == noteColumns)) {
      strcpy(previous, line);
      line[0] = '\0';
    }
    label(26,114,2,muted,"%s",previous);
    label(26,138,2,accent,"%s_",line);
    if (nameSaveFailed) label(26,158,2,muted,"Could not save notes");
    else if (length + 1 == sizeof(draftNotes)) label(26,158,2,muted,"Notes are full");
  } else {
    label(26,126,2,accent,"%s_",draftName);
    if (nameSaveFailed) label(26,154,2,muted,"Could not save name");
  }
  const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM-"};
  for (int row = 0; row < 4; ++row) {
    int length = strlen(rows[row]);
    for (int i = 0; i < length; ++i) {
      char c = rows[row][i];
      if (!uppercase && c >= 'A' && c <= 'Z') c += 'a' - 'A';
      char key[] = {c, '\0'};
      button(Action::Type,key,178+row*45,card,22+(10-length)*18+i*37,33,41,2,c);
    }
  }
  button(Action::Shift,uppercase ? "abc" : "ABC",362,card,22,71,46,2);
  button(Action::Type,"Space",362,card,98,94,46,2,' ');
  button(Action::Erase,"Del",362,card,197,forNotes ? 69 : 92,46,2);
  if (forNotes) {
    button(Action::Type,"Enter",362,card,271,74,46,1,'\n');
    button(Action::Clear,"Clr",362,card,350,38,46,1);
  } else button(Action::Clear,"Clear",362,card,294,94,46,2);
  button(forNotes ? Action::Notes : Action::Poses,"Cancel",417,card,22,174);
  button(forNotes ? Action::SaveNotes : Action::SaveName,"Save",417,blue,210,178);
}
void drawTimer(uint32_t now) {
  label(24,77,4,ink,"Timer");
  uint32_t remaining = timer.remaining(now);
  constexpr int x = 205, y = 224, radius = 103;
  canvas.fillCircle(x,y,radius,card);
  if (remaining == timer.duration()) canvas.fillCircle(x,y,radius,accent);
  else {
    int sweep = int(360 * remaining / timer.duration());
    for (int angle = 0; angle < sweep; angle += 6) {
      int end = std::min(angle + 6, sweep);
      canvas.fillTriangle(x,y,x+int(radius*sinf(angle*radians)),y-int(radius*cosf(angle*radians)),
                          x+int(radius*sinf(end*radians)),y-int(radius*cosf(end*radians)),accent);
    }
  }
  canvas.fillCircle(x,y,87,bg);
  unsigned seconds = (remaining + 999) / 1000;
  label(x-60,y-19,4,ink,"%u:%02u",seconds/60,seconds%60);
  if (!timer.running() && timer.minutes() > 1)
    button(Action::TimerAdjust,"-",194,card,24,63,58,4,-1);
  if (!timer.running() && timer.minutes() < 9)
    button(Action::TimerAdjust,"+",194,card,323,63,58,4,1);
  if (timer.finished()) label(151,260,2,accent,"Time's up");
  else label(151,260,2,muted,"Set %u min",timer.minutes());
  if (!timer.running()) button(Action::TimerStart,"Start",348,blue,22,174);
  else label(53,368,2,accent,"Running");
  button(Action::TimerStop,"Stop",348,card,210,178);
  button(Action::Home,"Back",417);
}
void drawAngles() {
  const Preset &preset = exercise();
  const char *const rows[] = {"Start of range", "Start concentric", "End of range", "Start eccentric"};
  label(24,77,3,ink,"Joint angles");
  label(26,119,2,ink,"%s",preset.name);
  label(26,153,2,muted,"Saved watch tilt (degrees)");
  label(34,193,2,muted,"Pose");
  label(235,193,2,muted,"Pitch");
  label(322,193,2,muted,"Roll");
  for (int i = 0; i < PoseCount; ++i) {
    int y = 217 + i * 43;
    canvas.fillRoundRect(22,y,366,39,8,card);
    label(34,y+12,2,ink,"%s",rows[i]);
    if (preset.saved[i]) {
      label(224,y+12,2,accent,"%+6.1f",pitch(preset.poses[i]));
      label(306,y+12,2,accent,"%+6.1f",roll(preset.poses[i]));
    } else {
      label(253,y+12,2,muted,"--");
      label(335,y+12,2,muted,"--");
    }
  }
  button(Action::Poses,"Back",417);
}
void drawCurlPose(int y, Pose pose, float bend) {
  constexpr int shoulderX = 67, elbowX = 69;
  int shoulderY = y + 16, elbowY = y + 34;
  int handX = elbowX + int(lroundf(22 * sinf(bend * radians)));
  int handY = elbowY + int(lroundf(22 * cosf(bend * radians)));
  canvas.fillCircle(49,y+12,6,muted);
  canvas.fillRoundRect(42,y+20,18,36,6,muted);
  canvas.drawFastHLine(54,shoulderY,shoulderX-53,muted);
  for (int offset = -2; offset <= 2; ++offset) {
    canvas.drawLine(shoulderX+offset,shoulderY,elbowX+offset,elbowY,accent);
    canvas.drawLine(elbowX,elbowY+offset,handX,handY+offset,accent);
    if (handX - elbowX < 12)
      canvas.drawLine(elbowX+offset,elbowY,handX+offset,handY,accent);
  }
  canvas.fillCircle(shoulderX,shoulderY,3,accent);
  canvas.fillCircle(elbowX,elbowY,4,accent);
  canvas.drawFastHLine(handX-8,handY,17,ink);
  canvas.fillRoundRect(handX-10,handY-5,5,11,2,ink);
  canvas.fillRoundRect(handX+6,handY-5,5,11,2,ink);
  canvas.fillCircle(handX,handY,3,accent);
  if (pose == Concentric || pose == Eccentric) {
    constexpr int arrowX = 111;
    int tip = y + (pose == Concentric ? 31 : 47);
    int tail = y + (pose == Concentric ? 47 : 31);
    int wing = tip + (pose == Concentric ? 5 : -5);
    canvas.drawLine(arrowX,tail,arrowX,tip,ink);
    canvas.drawLine(arrowX-5,wing,arrowX,tip,ink);
    canvas.drawLine(arrowX+5,wing,arrowX,tip,ink);
  }
}
void drawHelp() {
  const char *const captions[] = {"Arm straight", "Slight bend on lift", "Squeeze at the top", "Lower a few degrees"};
  constexpr float bends[] = {0, 22, 150, 142};
  label(24,77,3,ink,"Hammer curl");
  for (int i = 0; i < PoseCount; ++i) {
    int y = 115 + i * 72;
    canvas.fillRoundRect(22,y,366,66,10,card);
    drawCurlPose(y,Pose(i),bends[i]);
    label(128,y+13,2,ink,"%s",names[i] + 3);  // The diagrams share the calibration pose names.
    label(128,y+39,2,muted,"%s",captions[i]);
  }
  button(Action::Home,"Back",417);
}
void drawWireless() {
  using viewerstream::Status;
  displayedWireless = viewerstream::status();
  switch (displayedWireless) {
    case Status::Off:
      button(Action::Wireless,"Wireless  Off",230,card,22,366,52);
      break;
    case Status::Listening:
    case Status::Streaming:
      button(Action::Wireless,"Wireless  On",230,blue,22,366,52);
      label(26,291,2,muted,"%s",displayedWireless == Status::Streaming ?
            "Lift Studio connected" : "Join LiftWatch-S3");
      break;
    case Status::Starting:
    case Status::Stopping:
      label(26,248,2,muted,"%s",displayedWireless == Status::Starting ?
            "Starting wireless..." : "Stopping wireless...");
      break;
    case Status::Error:
      button(Action::Wireless,"Retry wireless",230,card,22,366,52);
      label(26,291,2,muted,"Wireless setup failed");
      break;
    case Status::Unavailable:
      label(26,248,2,muted,"Wireless unavailable");
      break;
  }
}
void draw(uint32_t now) {
  buttonCount = 0;
  canvas.fillScreen(bg);
  char status[32];
  if (battery >= 0) snprintf(status,sizeof(status),"RepSNS v0.1.0 / %d%%",battery);
  else snprintf(status,sizeof(status),"RepSNS v0.1.0 / --");
  constexpr int statusCharWidth = 12;  // The built-in 6 px font at text size 2.
  label((canvas.width() - int(strlen(status)) * statusCharWidth) / 2,24,2,accent,"%s",status);
  canvas.drawFastHLine(22,54,366,card);
  switch (page) {
    case Page::Startup: drawStartup(); break;
    case Page::Home: {
      label(24,72,4,ink,"RepSNS");
      orientation(111,true);
      constexpr int firstButtonY = 142, buttonHeight = 75, buttonStep = buttonHeight + 8;
      button(Action::Calibrate,"Before you track",firstButtonY,card,22,366,buttonHeight);
      button(Action::Apply,"Start tracking",firstButtonY+buttonStep,blue,22,366,buttonHeight);
      button(Action::Settings,"Settings",firstButtonY+2*buttonStep,card,22,174,buttonHeight);
      button(Action::Help,"Help",firstButtonY+2*buttonStep,card,210,178,buttonHeight);
      button(Action::Notes,"Notes",firstButtonY+3*buttonStep,card,22,174,buttonHeight);
      button(Action::Timer,"Timer",firstButtonY+3*buttonStep,card,210,178,buttonHeight);
      break;
    }
    case Page::Help: drawHelp(); break;
    case Page::Settings:
      label(24,77,4,ink,"Settings");
      label(26,129,2,muted,"Speaker volume");
      if (volume) label(310,129,2,accent,"%u%%",unsigned(volume));
      else label(310,129,2,accent,"Muted");
      if (volume > 0) button(Action::VolumeDown,"- 5%",155,card,22,174,48);
      if (volume < 100) button(Action::VolumeUp,"+ 5%",155,card,210,178,48);
      if (*volumeNotice) label(26,208,2,muted,"%s",volumeNotice);
      drawWireless();
      label(26,310,2,muted,"Wi-Fi on uses more battery.");
      button(Action::ResetPresets,"Reset exercise presets",334,red,22,366,48,2);
      if (*resetNotice) label(26,390,2,muted,"%s",resetNotice);
      button(Action::Home,"Back",417);
      break;
    case Page::ResetPresets:
      label(24,86,3,ink,"Reset all presets?");
      label(26,160,2,muted,"Delete all 8 exercise names,");
      label(26,188,2,muted,"poses, axes and tolerances.");
      button(Action::Settings,"Cancel",417,card,22,174,56,2);
      button(Action::ConfirmReset,"Reset all",417,red,210,178,56,2);
      break;
    case Page::Calibrate: drawPresets(true); break;
    case Page::Apply: drawPresets(false); break;
    case Page::Poses: {
      label(24,77,2,ink,"%s",exercise().name);
      button(Action::Name,"Edit name",108,blue,22,174,42,2);
      char axisTitle[20];
      snprintf(axisTitle, sizeof(axisTitle), "Axis: %s", axisName(exercise().tracking.axis));
      button(Action::Axis,axisTitle,108,card,210,178,42,2);
      for (int i = 0; i < PoseCount; ++i) {
        int y = 158 + i * 63;
        button(Action::Pose,names[i],y,card,22,366,54,2,i);
        if (exercise().saved[i]) label(336,y+19,2,accent,"OK");
      }
      button(Action::Calibrate,"Back",417,card,22,150,56,2);
      button(Action::Angles,"Joint angles",417,card,186,202,56,2);
      break;
    }
    case Page::Angles: drawAngles(); break;
    case Page::Axis: drawAxisMenu(); break;
    case Page::Tolerances: drawToleranceMenu(); break;
    case Page::Name: drawKeyboard(false); break;
    case Page::Notes: drawNotes(); break;
    case Page::NoteEdit: drawKeyboard(true); break;
    case Page::Timer: drawTimer(now); break;
    case Page::Loss: {
      label(24,77,2,ink,"%s",exercise().name);
      label(24,112,3,ink,"Velocity loss");
      for (int i = 0; i < int(sizeof(lossOptions) / sizeof(lossOptions[0])); ++i) {
        uint8_t loss = lossOptions[i];
        char title[10];
        snprintf(title, sizeof(title), "%u%%", unsigned(loss));
        button(Action::Loss,title,157 + (i/3)*57,loss == velocityLoss ? blue : card,
               22 + (i%3)*124,118,49,2,loss);
      }
      if (velocityLoss) label(26,273,2,muted,"Stop at %.1fx first rep time",1.0 + velocityLoss * .01);
      const char *issue = calibrationIssue(exercise());
      if (issue) {
        label(26,312,2,muted,"%s",issue);
        button(Action::OpenCalibration,"Capture poses",344,blue,22,366,54,2);
      } else if (setlog::busy()) label(26,312,2,muted,"Saving previous set...");
      else if (!fresh(now)) orientation(312);
      else if (!velocityLoss) label(26,312,2,muted,"Choose a velocity loss.");
      else button(Action::Start,"Start set",344,blue,22,366,54);
      button(Action::Apply,"Back",417);
      break;
    }
    case Page::PoseCountdown:
    case Page::Capture:
    case Page::SetCountdown:
      label(24,89,3,ink,"%s",page == Page::SetCountdown ? "Start set" : "Capture pose");
      label(26,151,2,muted,"%s",page == Page::SetCountdown ? "Get ready for the first rep" : names[selected]);
      if (page == Page::Capture) {
        label(26,225,4,accent,"Hold still");
        label(26,294,3,ink,"%.1f s",fmaxf(0, 3.0f - (now-activeSince)*0.001f));
      } else {
        label(28,209,10,accent,"%d",countdown);
        label(30,312,2,muted,"Get into position");
      }
      if (page == Page::SetCountdown) button(Action::Stop,"Emergency stop",417,red);
      else button(Action::Cancel,"Cancel",417);
      break;
    case Page::Result:
      label(24,110,3,ink,"%s",notice);
      label(26,186,2,muted,"%s",detail);
      if (exercise().saved[selected])
        label(26,239,2,accent,"Saved R %+.1f   P %+.1f",
              roll(exercise().poses[selected]),pitch(exercise().poses[selected]));
      button(Action::Poses,"Back to poses",417,blue);
      break;
    case Page::Live: {
      bool timing = session.phase == Phase::Timing;
      bool complete = session.count > 0 && session.phase == Phase::FindBottom;
      if (complete) label(24,78,3,accent,"Rep %lu complete",(unsigned long)session.count);
      else label(24,78,4,ink,"Rep %lu",(unsigned long)session.count+1);
      const char *phase = timing ? "Reach end range of motion" :
                          session.phase == Phase::Armed || !session.count ?
                          "Move through start concentric" : "Return toward the bottom";
      label(26,138,2,accent,"%s",phase);
      uint32_t duration = timing ? now-session.started : session.last;
      label(26,189,5,ink,"%.2f s",duration * .001);
      if (session.count) label(26,264,2,muted,"First %.2fs   Last %.2fs",session.first*.001,session.last*.001);
      if (timing) {
        TrackingAxis axis = exercise().tracking.axis;
        Vec top = exercise().poses[Top];
        if (axis == TrackingAxis::Both)
          label(26,294,2,muted,"End pose %.1f deg away",tiltDistance(latest, top));
        else {
          TrackingAxis secondary = otherAxis(axis);
          label(26,294,2,muted,"%s %.1f  %s %.1f deg",axisName(axis),poseDistance(latest, top, axis),
                axisName(secondary),poseDistance(latest, top, secondary));
        }
      }
      if (session.bottomHold.tracking)
        label(26,323,2,muted,"Bottom rest  %lu / 30 s",(unsigned long)(now-session.bottomHold.since)/1000);
      orientation(365);
      button(Action::Stop,"Emergency stop",417,red);
      break;
    }
    case Page::Summary: {
      label(24,78,4,ink,"Set ended");
      label(26,128,2,muted,"%s",notice);
      label(26,162,3,ink,"%lu reps / %.1f s",(unsigned long)session.count,setElapsed*.001);
      if (!times.empty()) {
        label(26,201,2,muted,"First %.2fs   Last %.2fs",session.first*.001,session.last*.001);
        label(26,228,2,muted,"Mean %.2fs    Best %.2fs",session.total*.001/session.count,session.fastest*.001);
        if (session.interruptedRepMs)
          label(26,255,2,accent,"Rep %lu stopped at %.2fs",
                (unsigned long)session.count + 1,session.interruptedRepMs*.001);
        else if (session.count > 1 && session.first)
          label(26,255,2,muted,"Last vs first  %+.1f%%",100.0*(double(session.last)/session.first-1.0));
        label(26,290,3,accent,"Rep %u  %.2f s",unsigned(review+1),times[review]*.001);
        if (review > 0) button(Action::Older,"Earlier",324,card,22,174,52,2);
        if (review+1 < times.size()) button(Action::Newer,"Later",324,card,210,178,52,2);
      }
      summarySaving = logStarted && setlog::busy();
      auto status = setlog::status();
      if (summarySaving) label(26,390,2,muted,"Saving SD log...");
      else if (logStarted && status == setlog::Status::Saved)
        label(26,390,2,accent,"Saved set%06lu.csv",(unsigned long)setlog::fileNumber());
      else if (logStarted && status == setlog::Status::Partial)
        label(26,390,2,muted,"Partial SD log");
      else if (setElapsed || logStarted) label(26,390,2,muted,"SD log unavailable");
      button(Action::Apply,"Done",417,blue);
      break;
    }
    case Page::Fault:
      label(24,102,3,ink,"Watch setup");
      label(26,171,2,muted,"%s",notice);
      label(26,217,2,muted,"Check watch and restart.");
      break;
  }
  canvas.flush();
  lastRender = millis();
  redraw = false;
}

void activate(Button tapped, uint32_t now) {
  switch (tapped.action) {
    case Action::Home: show(Page::Home); break;
    case Action::Help:
      audio::play(audio::Cue::Help);
      show(Page::Help);
      break;
    case Action::Settings:
      if (page == Page::Home) audio::play(audio::Cue::Settings);
      show(Page::Settings);
      break;
    case Action::Wireless: viewerstream::toggle(); redraw = true; break;
    case Action::ResetPresets: show(Page::ResetPresets); break;
    case Action::ConfirmReset:
      resetPresets();
      show(Page::Settings);
      break;
    case Action::VolumeDown:
    case Action::VolumeUp: {
      int change = tapped.action == Action::VolumeUp ? 5 : -5;
      uint8_t next = constrain(int(volume) + change, 0, 100);
      if (board::speakerVolume(next)) {
        volume = next;
        volumeNotice = nvm.putUChar("volume", volume) ? "" : "Volume could not be saved";
      } else {
        volumeNotice = "Volume change failed";
      }
      redraw = true;
      break;
    }
    case Action::Calibrate:
      if (page == Page::Home) {
        presetPage = 0;
        audio::play(audio::Cue::Calibrate);
      }
      show(Page::Calibrate);
      break;
    case Action::Apply:
      if (page == Page::Home) {
        presetPage = 0;
        audio::play(audio::Cue::StartTracking);
      }
      show(Page::Apply);
      break;
    case Action::Slot:
      slot = tapped.value;
      velocityLoss = 0;
      show(page == Page::Calibrate ? Page::Poses : Page::Loss);
      break;
    case Action::PresetPage: presetPage = 1 - presetPage; redraw = true; break;
    case Action::OpenCalibration:
      audio::play(audio::Cue::Calibrate);
      show(Page::Poses);
      break;
    case Action::Poses: show(Page::Poses); break;
    case Action::Angles: show(Page::Angles); break;
    case Action::Axis:
      axisNotice = "";
      show(Page::Axis);
      break;
    case Action::SaveAxis: {
      TrackingAxis axis = TrackingAxis(tapped.value);
      char key[16];
      axisKey(key, slot);
      if (axis != exercise().tracking.axis && !nvm.putUChar(key, uint8_t(axis))) {
        axisNotice = "Could not save tracking axis";
      } else {
        exercise().tracking.axis = axis;
        axisNotice = "";
      }
      redraw = true;
      break;
    }
    case Action::Tolerances:
      axisNotice = "";
      show(Page::Tolerances);
      break;
    case Action::MainDegrees:
    case Action::OffDegrees: {
      TrackingSettings next = exercise().tracking;
      if (tapped.action == Action::MainDegrees) next.mainDegrees = tapped.value;
      else next.offDegrees = tapped.value;
      char key[16];
      toleranceKey(key,slot);
      uint16_t packed = (uint16_t(next.mainDegrees) << 8) | next.offDegrees;
      if (nvm.putUShort(key, packed)) {
        exercise().tracking = next;
        axisNotice = "";
      } else axisNotice = "Could not save tolerances";
      redraw = true;
      break;
    }
    case Action::Notes:
      if (page == Page::Home) {
        notePage = 0;
        audio::play(audio::Cue::Notes);
      }
      show(Page::Notes);
      break;
    case Action::NotePage: notePage += tapped.value; redraw = true; break;
    case Action::NoteEdit:
      strcpy(draftNotes,notes);
      uppercase = true;
      nameSaveFailed = false;
      show(Page::NoteEdit);
      break;
    case Action::SaveNotes:
      if (strcmp(draftNotes, notes) &&
          !(*draftNotes ? nvm.putString("notes",draftNotes) == strlen(draftNotes) : removeSavedKey("notes"))) {
        nameSaveFailed = true;
        redraw = true;
        break;
      }
      strcpy(notes,draftNotes);
      notePage = 0;
      show(Page::Notes);
      break;
    case Action::Timer:
      if (page == Page::Home) audio::play(audio::Cue::Timer);
      show(Page::Timer);
      break;
    case Action::TimerAdjust: timer.adjust(tapped.value); redraw = true; break;
    case Action::TimerStart: timer.start(now); redraw = true; break;
    case Action::TimerStop: timer.stop(now); redraw = true; break;
    case Action::Name:
      strcpy(draftName, exercise().name);
      uppercase = true;
      nameSaveFailed = false;
      show(Page::Name);
      break;
    case Action::Type: {
      char *draft = page == Page::NoteEdit ? draftNotes : draftName;
      size_t capacity = page == Page::NoteEdit ? sizeof(draftNotes) : sizeof(draftName);
      size_t length = strlen(draft);
      if (length + 1 < capacity) {
        draft[length] = char(tapped.value);
        draft[length + 1] = '\0';
        redraw = true;
      }
      break;
    }
    case Action::Shift: uppercase = !uppercase; redraw = true; break;
    case Action::Clear:
      (page == Page::NoteEdit ? draftNotes : draftName)[0] = '\0';
      redraw = true;
      break;
    case Action::Erase: {
      char *draft = page == Page::NoteEdit ? draftNotes : draftName;
      size_t length = strlen(draft);
      if (length) draft[length - 1] = '\0';
      redraw = true;
      break;
    }
    case Action::SaveName: {
      const char *first = draftName + strspn(draftName, " ");
      size_t length = strlen(first);
      while (length && first[length - 1] == ' ') --length;
      char name[sizeof(draftName)];
      if (length) snprintf(name, sizeof(name), "%.*s", int(length), first);
      else snprintf(name, sizeof(name), "Exercise %d", slot + 1);
      char key[16];
      nameKey(key, slot);
      if (strcmp(name, exercise().name) && !nvm.putString(key, name)) {
        nameSaveFailed = true;
        redraw = true;
        break;
      }
      strcpy(exercise().name, name);
      show(Page::Poses);
      break;
    }
    case Action::Loss: velocityLoss = tapped.value; redraw = true; break;
    case Action::Pose:
      selected = tapped.value;
      if (fresh(now)) {
        audio::play(audio::Cue(int(audio::Cue::Bottom) + selected));
        startCountdown(Page::PoseCountdown, now);
      } else if (board::imuCalibrationProgress() < 0)
        captureResult("IMU data unavailable","Check IMU status on Home.");
      else captureResult("IMU warming up","Hold still, then try again.");
      break;
    case Action::Start:
      if (!velocityLoss || !fresh(now) || calibrationIssue(exercise()) || setlog::busy()) break;
      times.clear();
      session = RepSet(exercise().poses, exercise().tracking);
      logStarted = false;
      session.velocityLoss = velocityLoss;
      startCountdown(Page::SetCountdown,now);
      break;
    case Action::Stop: endSet("Stopped by user",now); break;
    case Action::Cancel: silence(); audio::play(audio::Cue::Silence); show(Page::Poses); break;
    case Action::Older: if (review) --review; redraw = true; break;
    case Action::Newer: if (review+1 < times.size()) ++review; redraw = true; break;
    default: break;
  }
}
void handleTouch(uint32_t now) {
  if (now-lastTouch < 15) return;
  lastTouch = now;
  bool pressed;
  int x,y;
  if (!board::touch(pressed,x,y)) return;
  if (pressed && !pressedBefore) {
    for (int i=0; i<buttonCount; ++i) {
      const Button &b = buttons[i];
      if (x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h) {
        activate(b,now);
        // A countdown or rep alert can also acknowledge the tap.
        if (!pulsing || millis() - pulseStart >= pulseLength) pulse(menuPulseMs);
        break;
      }
    }
  }
  pressedBefore = pressed;
}
}  // namespace

void watchBegin() {
  Serial.begin(115200);
  const char *error = board::begin();
  boardReady = !error;
  screenReady = canvas.begin();
  if (!screenReady) { Serial.println("Display setup failed. Check OPI PSRAM."); return; }
  canvas.setTextWrap(false);
  if (!error && !nvm.begin("repwatch-v1",false)) error = "Storage setup failed";
  if (error) { notice = error; Serial.println(error); show(Page::Fault); return; }
  uint8_t savedVolume = nvm.getUChar("volume", audio::defaultVolume);
  if (savedVolume <= 100 && savedVolume != audio::defaultVolume) {
    if (board::speakerVolume(savedVolume)) volume = savedVolume;
    else volumeNotice = "Saved volume could not load";
  }
  for (int index = 0; index < presetCount; ++index) loadPreset(index);
  if (nvm.isKey("notes")) nvm.getString("notes",notes,sizeof(notes));
  battery = board::batteryPercent();
  setlog::begin();
  lastBattery = rateStart = millis();
  draw(lastBattery);
  if (!board::startImu()) {
    notice = "IMU task failed";
    Serial.println(notice);
    show(Page::Fault);
  } else if (!viewerstream::begin()) Serial.println("Viewer Wi-Fi unavailable");
}
void watchUpdate() {
  if (!screenReady) { delay(50); return; }
  if (page == Page::Fault) { if (redraw) draw(millis()); delay(20); return; }
  uint32_t now = millis();
  tickCountdown(now);
  board::Sample sample;
  while (board::pop(sample)) {
    acceptSample(sample);
    viewerstream::submit(sample);
  }
  now = millis();
  if (timer.update(now) && page == Page::Timer) redraw = true;
  if (page == Page::Startup) updateStartup(now);
  if ((posePage() || setPage()) && !fresh(now)) sensorGap();
  handleTouch(now);
  if (pulsing && millis()-pulseStart >= pulseLength) pulsing = false;
  uint32_t motorNow = millis();
  board::motor(timer.alarming(motorNow) ? timer.buzz(motorNow) : pulsing);
  if (page == Page::Summary && summarySaving && !setlog::busy()) redraw = true;
  if (page == Page::Settings && displayedWireless != viewerstream::status()) redraw = true;
  if (now-rateStart >= 1000) {
    sampleHz = int(1000.0f*rateCount/(now-rateStart)+0.5f);
    rateStart = now;
    rateCount = 0;
  }
  if (boardReady && now-lastBattery >= 5000) {
    int value = board::batteryPercent();
    if (value != battery) redraw = true;
    battery = value;
    lastBattery = now;
  }
  bool dynamic = page == Page::Startup || page == Page::Home || page == Page::Loss ||
                 page == Page::Capture || page == Page::Live || (page == Page::Timer && timer.running());
  if (redraw || (dynamic && now-lastRender >= 125)) draw(millis());
  delay(1);
}
