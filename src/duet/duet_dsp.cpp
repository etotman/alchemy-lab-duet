/**
 * duet_dsp.cpp — the handpan engine, compiled in place.
 *
 * The Makefile builds only the .cpp files in src/$(FW)/, so the DSP has to appear in this
 * folder to be linked. It is included rather than copied so there is one
 * engine, shared with the author's single-module handpan firmwares. Its own #include
 * "handpan_dsp.h" resolves next to it in src/handpan/.
 */

#include "handpan/handpan_dsp.cpp"
