#pragma once
// Camera stabilizer: removes the shake of Smooth Blend camera markers in exported videos.
//
// Why the camera shakes: the Rockstar Editor's Smooth Blend is a straight line between the markers with a damped
// spring on top, advanced every frame by a time step. In the editor that step is how far the replay moved. During an
// export the replay moves by a fixed tiny step (1 / fps / (samples + 1)), but the spring is still given the REAL time
// the frame took (rendering + encoding + streaming), which is many times longer and different on every frame - so the
// spring jumps around and the camera shakes in the video, never in the editor. (Explained by EVEM - Extended Video
// Export Modified - whose README describes this; this is our own implementation of the idea.)
//
// What this does, only while an export runs: it finds camReplayDirector::UpdateSmoothing, looks at the timer values
// that function reads, works out which of them follow the real frame time, and for the duration of that one call
// gives them the export step instead. Everything else in the game keeps its real clock. The hook is put in when an
// export starts and taken out when it ends, so outside exports the game code is untouched.

#include <cstdint>

namespace stabilizer {
void Begin(double step_seconds); // export starts (EVE1G's export step for one game frame)
void End();                      // export ended: unhook, write a summary to the log
} // namespace stabilizer
