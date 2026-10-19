// Build switches (edit before building; a release build leaves both at 0).
//   DS2_DEVELOPER = 1: developer tools: remote control file (ds2upscale.cfg), status file, frame dumps, GPU timers.
//   DS2_PROFILE   = 1: CPU probes around every event handler and the render thread's CPU time per frame (status file line "cpu:").
#pragma once

#define DS2_VERSION "1.1.7"

#ifndef DS2_DEVELOPER
#define DS2_DEVELOPER 0
#endif
#ifndef DS2_PROFILE
#define DS2_PROFILE DS2_DEVELOPER
#endif



