#pragma once

// vsthost --bridge: serves /dev/vstbridge (real-time hosting, see bridge.cpp).
int runBridge();

// vsthost --replay <plugin> <capture> <out.f32>: renders a recorded request
// stream offline with the bridge's own processing path (the reference for the
// real-time sample-identity test).
int runReplay(int argc, char **argv);
