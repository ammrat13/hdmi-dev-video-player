#include "hdmi_dev.h"
#include "hdmi_fb.h"
#include "video.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

//! \brief Print the usage and exit
//! \details Exits with code 1
__attribute__((noreturn)) void usage(void) {
  const char *const USAGE =
      "Usage: hdmi-dev-video-player [VIDEO] [FDIV]\n"
      "Plays the video file specified by [VIDEO] using the HDMI Peripheral\n"
      "with the frame-rate divider [FDIV]\n"
      "\n"
      "The input video must be 640x480, and it must have frames encoded as\n"
      "YUV420P. It also cannot have any audio associated with it - it must be\n"
      "a single stream.\n"
      "\n"
      "The frame-rate divider is applied to a 60Hz refresh rate. In other\n"
      "words, the frame rate is (60Hz / [FDIV]). Setting the divider too low\n"
      "will cause frames to miss their deadline and for the video to be\n"
      "played back slower. A stable value is [FDIV] = 3.\n"
      "\n"
      "Finally, this program must be used with the HDMI Peripheral. It must\n"
      "be run as root to interact with the device.\n";
  fputs(USAGE, stderr);
  exit(1);
}

//! \brief Stop the device and exit
//!
//! This method only waits for the device to signal completion on SIGINT.
//! Otherwise, it just stops the device and exits. It also bypasses all the
//! atexit hooks. It is intended to installed with `sigaction`.
__attribute__((noreturn)) void signal_handler(int signum) {
  if (signum == SIGINT)
    hdmi_dev_stop();
  else
    hdmi_dev_stopnow();
  _exit(2);
}

int main(int argc, char **argv) {

  // Check if the user is asking for help
  if (argc == 2 && strcmp("help", argv[1]) == 0)
    usage();
  else if (argc == 2 && strcmp("--help", argv[1]) == 0)
    usage();
  // Check for correct usage
  if (argc != 3) {
    fputs("Usage: wrong number of arguments\n", stderr);
    usage();
  } else if (geteuid() != 0) {
    fputs("Usage: must be run as root\n", stderr);
    usage();
  }

  // Parse the frame-rate divider
  const int FDIV = atoi(argv[2]);
  if (FDIV <= 0) {
    fputs("Usage: invalid frame-rate divider\n", stderr);
    usage();
  }

  // Open the video to play
  video_t *vid = video_open(argv[1]);
  if (vid == NULL) {
    fputs("Usage: failed to open video\n", stderr);
    usage();
  }

  // Create the framebuffer allocator ...
  hdmi_fb_allocator_t *alloc_fb = hdmi_fb_allocator_open();
  if (alloc_fb == NULL) {
    fputs("Error: failed to open framebuffer allocator\n", stderr);
    exit(127);
  }
  // ... so we can allocate two framebuffers to double-buffer with
  hdmi_fb_handle_t *fbs[2];
  for (size_t i = 0u; i < 2u; i++) {
    fbs[i] = hdmi_fb_allocate(alloc_fb);
    if (fbs[i] == NULL) {
      fputs("Error: failed to allocate framebuffer\n", stderr);
      exit(127);
    }
  }

  // Setup the SIGINT and SIGTERM handlers
  {
    const struct sigaction args = {.sa_handler = signal_handler};
    int res_int = sigaction(SIGINT, &args, NULL);
    int res_term = sigaction(SIGTERM, &args, NULL);
    if (res_int != 0 || res_term != 0) {
      fputs("Error: couldn't setup signal handler\n", stderr);
      exit(127);
    }
  }

  // Setup the device
  if (!hdmi_dev_open()) {
    fputs("Error: failed to open HDMI Peripheral\n", stderr);
    exit(127);
  }

  puts("TRACE: Done with setup!");

  // Keep reading frames until we hit the end of the file
  size_t fb = 0u;
  hdmi_coordinate_t last;
  bool first = true;
  while (true) {

    // Decode a frame into the current framebuffer. This will switch because
    // we're double buffering.
    int res = video_get_frame(vid, hdmi_fb_data(fbs[fb]));
    if (res == AVERROR_EOF) {
      fputs("TRACE: Hit EOF on video\n", stderr);
      break;
    } else if (res != 0) {
      fprintf(stderr, "Error: got %d when decoding video\n", res);
    }
    // Remember to flush the framebuffer from the cache before presenting
    hdmi_fb_flush(alloc_fb, fbs[fb]);

    if (first) {
      // If this is our first frame, we can just immediately present it. We also
      // have to start the device, and set the coordinate on which we presented
      // the frame so the next iteration can use it.
      hdmi_dev_set_fb(fbs[fb]);
      hdmi_dev_start();
      last = hdmi_dev_coordinate();

    } else {
      // Wait until enough frames have elapsed since the last one before
      // presenting this frame. The procedure is a bit complicated - we need to
      // make sure the HDMI Peripheral has in fact switched to the new
      // framebuffer before we clobber the old one. Specifically, we need to
      // tell the device about the new framebuffer before `FDIV` frames have
      // elapsed, then we need to wait for `FDIV` frames to actually elapse
      // before continuing.

      // We'll use this variable throughout this section to keep track of where
      // the device is currently
      hdmi_coordinate_t cur = hdmi_dev_coordinate();
      int_fast16_t fid_delta = hdmi_fid_delta(cur.fid, last.fid);

      // Check that we actually met the deadline. We need some margin before we
      // have to present, so we'll make sure we're still before the last line on
      // the frame before. 31us should be plenty.
      bool overshoot_frame = fid_delta >= FDIV;
      bool overshoot_line = fid_delta == FDIV - 1 && cur.row >= 524u;
      if (overshoot_frame || overshoot_line)
        fputs("WARN: missed deadline\n", stderr);

      // Wait until we're on the frame just before we have to present. Remember
      // to update the state variables.
      while (fid_delta < FDIV - 1) {
        cur = hdmi_dev_coordinate();
        fid_delta = hdmi_fid_delta(cur.fid, last.fid);
      }
      // Give the peripheral the new framebuffer
      hdmi_dev_set_fb(fbs[fb]);
      // The new framebuffer won't start being used until the start of the next
      // frame, so wait for that
      while (fid_delta < FDIV) {
        cur = hdmi_dev_coordinate();
        fid_delta = hdmi_fid_delta(cur.fid, last.fid);
      }

      // Remember to update the coordinate for the next loop
      last = cur;
    }

    // Next
    fb = (fb + 1u) & 1u;
    first = false;
  }

  // At least cleanup on the happy path
  puts("TRACE: Cleaning up...");
  hdmi_dev_stop();
  hdmi_dev_close();
  hdmi_fb_free(alloc_fb, fbs[0u]);
  hdmi_fb_free(alloc_fb, fbs[1u]);
  hdmi_fb_allocator_close(alloc_fb);
  video_close(vid);
  puts("TRACE: Cleaned up!");
  return 0;
}
