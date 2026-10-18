#!/usr/bin/env python3
"""Hooks port/kbreplay.m into a scratch copy of the tree (port/kbreplay.sh; never the work tree).

- code/client/kbreplay.m: the replay command and the event log, in the client of both input layers;
- IN_ProcessEvents (sdl_input.c and macos_input.m): KbReplay_Frame() right before the pump; IN_Shutdown:
  KbReplay_Shutdown() first (marks the log: the window goes away);
- Com_QueueEvent (common.c): KbReplay_LogEvent() for every queued event (a weak empty default, so the dedicated
  server, which has no client, still links).

Usage: kbreplay_patch.py <scratch tree>
"""
import os
import shutil
import sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def patch(rel, old, new, count=1):
    path = os.path.join(root, rel)
    with open(path, newline="") as f:
        text = f.read()
    if text.count(old) != count:
        sys.exit("kbreplay_patch.py: %s: expected %d match(es), found %d:\n%s" % (rel, count, text.count(old), old))
    with open(path, "w", newline="") as f:
        f.write(text.replace(old, new))


shutil.copy(os.path.join(here, "kbreplay.m"), os.path.join(root, "code/client/kbreplay.m"))

patch("cmake/client.cmake", "\nset(CLIENT_SOURCES\n",
      "\n# kbreplay (scratch): both input layers\n"
      "list(APPEND CLIENT_INPUT_SOURCES ${SOURCE_DIR}/client/kbreplay.m)\n"
      "set_source_files_properties(${SOURCE_DIR}/client/kbreplay.m PROPERTIES COMPILE_OPTIONS \"-fobjc-arc\")\n"
      "list(APPEND CLIENT_LIBRARIES \"-framework Carbon\")\n"
      "\nset(CLIENT_SOURCES\n")

for rel in ("code/sdl/sdl_input.c", "code/macos/macos_input.m"):
    patch(rel, '#include "../sys/sys_local.h"\n',
          '#include "../sys/sys_local.h"\n\nvoid KbReplay_Frame( void );\nvoid KbReplay_Shutdown( void );\n')
    patch(rel, "\tCL_LatencyPump( );\n", "\tKbReplay_Frame( );\n\n\tCL_LatencyPump( );\n")
    patch(rel, "void IN_Shutdown( void )\n{\n", "void IN_Shutdown( void )\n{\n\tKbReplay_Shutdown( );\n\n")

patch("code/qcommon/common.c",
      "void Com_QueueEvent( int time, sysEventType_t type, int value, int value2, size_t ptrLength, void *ptr )\n{\n"
      "\tsysEvent_t  *ev;\n",
      "__attribute__((weak)) void KbReplay_LogEvent( int type, int value, int value2 ) {}\n\n"
      "void Com_QueueEvent( int time, sysEventType_t type, int value, int value2, size_t ptrLength, void *ptr )\n{\n"
      "\tsysEvent_t  *ev;\n\n\tKbReplay_LogEvent( type, value, value2 );\n")

print("kbreplay_patch.py: patched", root)
