/*
 * GStreamer
 * Copyright (C) 2012,2013 Duzy Chan <code@duzy.info>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*! @file */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstcamcontrol_canon.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

G_DEFINE_TYPE (GstCamControllerCanon, gst_cam_controller_canon,
    GST_TYPE_CAM_CONTROLLER);

// @fixme: Better using libvisca, but I can't see this is working with
//         my camera.

//
#define CANON_COMMAND                    0x01
#define CANON_CATEGORY_CAMERA1           0x04
#define CANON_CATEGORY_CAMERA2           0x07
#define CANON_CATEGORY_PAN_TILTER        0x06
#define CANON_TERMINATOR                 0xFF

//
#define CANON_ZOOM                       0x07
#define CANON_ZOOM_STOP                  0x00
#define CANON_ZOOM_TELE                  0x02
#define CANON_ZOOM_WIDE                  0x03
#define CANON_ZOOM_TELE_SPEED            0x20
#define CANON_ZOOM_WIDE_SPEED            0x30
#define CANON_PT_ABSOLUTE_POSITION       0x02
#define CANON_PT_RELATIVE_POSITION       0x03
#define CANON_PT_HOME                    0x04
#define CANON_PT_RESET                   0x05
#define CANON_PT_LIMITSET                0x07

#define canon_message_init(a) { {0}, 0, (a) }

typedef struct _canon_message
{
  char buffer[256];             // 32 bytes for one command max
  int len;
  int address;
} canon_message;

static void
canon_message_append (canon_message * msg, char c)
{
  msg->buffer[msg->len++] = c;
}

static void
canon_message_reset (canon_message * msg)
{
  msg->len = 0;
  bzero (msg->buffer, sizeof (msg->buffer));
}

static void
canon_message_dump (const canon_message * msg, const gchar * tag)
{
  int n;

  g_print ("%s: ", tag);
  if (msg->len <= 0) {
    g_print ("(empty message)\n");
    return;
  }

  for (n = 0; n < msg->len; ++n) {
    int c = msg->buffer[n];
    if (c == '\r')
      g_print ("\\r");
    /*
       else if (isprint (c))
       g_print ("%c", c);
     */
    else
      g_print ("\\x%02X", (unsigned char) c);
  }
  g_print ("\n");
}

static gboolean
canon_message_send (int fd, const canon_message * msg)
{
  int len = 1 + msg->len + 1, n;
  char b[32];
  if (msg->len <= 0 || sizeof (b) <= len || 7 < msg->address) {
    g_print ("canon_message_send: %d, %d", msg->len, msg->address);
    return FALSE;
  }

  b[0] = 0x80;
  b[0] |= (msg->address << 4);
#if 0
  if (0 < broadcast) {
    b[0] |= (broadcast << 3);
    b[0] &= 0xF8;
  } else {
    b[0] |= camera_address;
  }
#endif

  memcpy (&b[1], msg->buffer, msg->len);
  b[1 + msg->len] = CANON_TERMINATOR;

  n = write (fd, msg->buffer, msg->len);
  if (n < msg->len) {
    g_print ("wrote: %d != %d\n", n, msg->len);
    return FALSE;
  }

  canon_message_dump (msg, "sent");
  //g_print ("wrote: %d, %d\n", n, msg->len);
  return TRUE;
}

/*
static gboolean
canon_message_reply (int fd, canon_message * reply)
{
  int available_bytes = 0, n = 0;

  do {
    ioctl (fd, FIONREAD, &available_bytes);
    usleep (500);
  } while (available_bytes == 0);

  do {
    if (read (fd, &reply->buffer[n], 1) != 1) {
      return FALSE;
    }
    if (reply->buffer[n] == CANON_TERMINATOR) {
      break;
    }
    n += 1;
    usleep (1);
  } while (n < sizeof (reply->buffer) - 1);

  return TRUE;
}
*/

static gboolean
canon_message_send_with_reply (int fd, const canon_message * msg,
    canon_message * reply)
{
  if (!canon_message_send (fd, msg)) {
    return FALSE;
  }
  //return canon_message_reply (fd, reply);
  return TRUE;
}

static void
gst_cam_controller_canon_init (GstCamControllerCanon * canon)
{
  canon->fd = -1;
  canon->zoom_speed = 100;
  canon->pan_speed = 100;
  canon->tilt_speed = 100;

  bzero (&canon->options, sizeof (canon->options));
}

static void
gst_cam_controller_canon_finalize (GstCamControllerCanon * canon)
{
  G_OBJECT_CLASS (gst_cam_controller_canon_parent_class)
      ->finalize (G_OBJECT (canon));
}

static void
gst_cam_controller_canon_close (GstCamControllerCanon * canon)
{
  g_print ("canon: close()\n");

  if (0 < canon->fd) {
    close (canon->fd);
    canon->fd = -1;

    g_free ((void *) canon->device);
    canon->device = NULL;

    bzero (&canon->options, sizeof (canon->options));
  }
}

static gboolean
gst_cam_controller_canon_open (GstCamControllerCanon * canon, const char *dev)
{
  canon_message msg = canon_message_init (0);
  canon_message reply = canon_message_init (0);

  g_print ("canon: open(%s)\n", dev);

  if (0 < canon->fd) {
    gst_cam_controller_canon_close (canon);
  }

  g_free ((void *) canon->device);
  canon->device = g_strdup (dev);
  canon->fd = open (dev, O_RDWR | O_NDELAY | O_NOCTTY);

  if (canon->fd == -1) {
    g_print ("canon: open(%s) error: %s\n", dev, strerror (errno));
    return FALSE;
  }

  fcntl (canon->fd, F_SETFL, 0);
  /* Setting port parameters */
  tcgetattr (canon->fd, &canon->options);

  /**
 RS-232C Conformity Connector & Pin assignment of connector are referred to 2.2 
 Transmission Mode : Half Duplex (Full duplex for notification) 
 Transfer Speed : 4800, 9600, 14400, 19200bps. (selected through menu window) 
 Data Bit : 8 bit 
 Parity : None 
 Stop Bit : 1 bit or 2 bit (selected through menu window) 
 Handshake : RTS/CTS Control 
   */
  /* control flags */
  //cfsetispeed(&canon->options, B2400);
  //cfsetispeed(&canon->options, B4800);
  //cfsetispeed (&canon->options, B9600);
  cfsetispeed (&canon->options, B19200);
  //cfsetispeed(&canon->options, B38400);
  //cfsetispeed(&canon->options, B57600);
  //cfsetispeed(&canon->options, B115200);
  //cfsetispeed(&canon->options, B230400);
  cfsetospeed (&canon->options, B9600);
  canon->options.c_cflag &= ~PARENB;    /* No parity  */
  canon->options.c_cflag &= ~CSTOPB;    /*  one stop bit          */
  //canon->options.c_cflag |= CSTOPB;     /*  two stop bits         */
  canon->options.c_cflag &= ~CSIZE;     /* 8bit       */
  canon->options.c_cflag |= CS8;        /*            */
  //canon->options.c_cflag &= ~CRTSCTS;   /* No hdw ctl */
  canon->options.c_cflag |= CRTSCTS;    /* Enable  RTS/CTS */

  /* local flags */
  canon->options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);    /* raw input */

  /* input flags */
  /*
     canon->options.c_iflag &= ~(INPCK | ISTRIP); // no parity
     canon->options.c_iflag &= ~(IXON | IXOFF | IXANY); // no soft ctl
   */
  /* patch: bpflegin: set to 0 in order to avoid invalid pan/tilt return values */
  canon->options.c_iflag = 0;

  /* output flags */
  canon->options.c_oflag &= ~OPOST;     /* raw output */

  tcsetattr (canon->fd, TCSANOW, &canon->options);

  //////////////////////////////////////////////////

  //FF 30 30 00 86 30 EF
  //FF 30 30 00 58 30 EF
  //FF 30 30 00 91 33 30 30 30 31 30 32 EF
  //FF 30 30 00 91 35 31 32 33 34 35 36 EF
  //FF 30 30 00 86 30 EF

  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x86);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0xEF);

  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x58);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0xEF);

  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x91);
  canon_message_append (&msg, 0x33);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x31);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x32);
  canon_message_append (&msg, 0xEF);

  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x91);
  canon_message_append (&msg, 0x35);
  canon_message_append (&msg, 0x31);
  canon_message_append (&msg, 0x32);
  canon_message_append (&msg, 0x33);
  canon_message_append (&msg, 0x34);
  canon_message_append (&msg, 0x35);
  canon_message_append (&msg, 0x36);
  canon_message_append (&msg, 0xEF);

  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x86);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0xEF);

  canon_message_send_with_reply (canon->fd, &msg, &reply);
  return TRUE;
}

static gboolean
gst_cam_controller_canon_pan (GstCamControllerCanon * canon, double speed,
    double v)
{
  canon_message msg = canon_message_init (0);
  canon_message reply = canon_message_init (0);
  guint uspeed = speed * 0x320;
  char buf[10] = { 0 };

  //008~320h
  if (uspeed < 0x8)
    uspeed = 0x8;
  if (0x320 < uspeed)
    uspeed = 0x320;

  g_print ("canon: pan(%f, %f)\n", speed, v);

  sprintf (buf, "%3X", uspeed);
  g_print ("canon: pan: %s, %x\n", buf, uspeed);

  //FFh 30h 3Xh 00h 51h p0 p1 p2 EFh
  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x50);
  canon_message_append (&msg, buf[0]);
  canon_message_append (&msg, buf[1]);
  canon_message_append (&msg, buf[2]);
  canon_message_append (&msg, 0xEF);

  //FFh 30h 3Xh 00h 53h 33h EFh
  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x53);
  if (v < 0) {
    canon_message_append (&msg, 0x02);
  } else {
    canon_message_append (&msg, 0x01);
  }
  canon_message_append (&msg, 0xEF);

  return canon_message_send_with_reply (canon->fd, &msg, &reply);
}

static gboolean
gst_cam_controller_canon_tilt (GstCamControllerCanon * canon, double speed,
    double v)
{
  canon_message msg = canon_message_init (0);
  canon_message reply = canon_message_init (0);
  guint uspeed = speed * 0x320;
  char buf[10] = { 0 };

  //008~320h
  if (uspeed < 0x8)
    uspeed = 0x8;
  if (0x320 < uspeed)
    uspeed = 0x320;

  g_print ("canon: tilt(%f, %f)\n", speed, v);

  sprintf (buf, "%3X", uspeed);
  g_print ("canon: tilt: %s, %x\n", buf, uspeed);

  //FFh 30h 3Xh 00h 51h p0 p1 p2 EFh
  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x51);
  canon_message_append (&msg, buf[0]);
  canon_message_append (&msg, buf[1]);
  canon_message_append (&msg, buf[2]);
  canon_message_append (&msg, 0xEF);

  //FFh 30h 3Xh 00h 53h 33h EFh
  canon_message_append (&msg, 0xFF);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x30);
  canon_message_append (&msg, 0x00);
  canon_message_append (&msg, 0x53);
  if (v < 0) {
    canon_message_append (&msg, 0x03);
  } else {
    canon_message_append (&msg, 0x04);
  }
  canon_message_append (&msg, 0xEF);

  return canon_message_send_with_reply (canon->fd, &msg, &reply);
}

static gboolean
gst_cam_controller_canon_move (GstCamControllerCanon * canon, double speed,
    double x, double y)
{
  canon_message msg = canon_message_init (0);
  canon_message reply = canon_message_init (0);

  g_print ("canon: move(%f, %f)\n", x, y);

  // ...
  return canon_message_send_with_reply (canon->fd, &msg, &reply);
}

/**
 *  @brief Zooming camera.
 *  @param z Zero means stop zooming,
 *         z < 0 means TELE zoom,
 *         0 < z means WIDE zoom
 *  @return TRUE if commands sent.
 */
static gboolean
gst_cam_controller_canon_zoom (GstCamControllerCanon * canon, double speed,
    double z)
{
  canon_message msg = canon_message_init (0);
  canon_message reply = canon_message_init (0);

  g_print ("canon: zoom(%f)\n", z);

  canon_message_append (&msg, CANON_COMMAND);
  canon_message_append (&msg, CANON_CATEGORY_CAMERA1);
  canon_message_append (&msg, CANON_ZOOM);
  if (z == 0) {
    canon_message_append (&msg, CANON_ZOOM_STOP);
  } else if (z < 0) {
    canon_message_append (&msg,
        CANON_ZOOM_TELE_SPEED | ((int) speed /*canon->zoom_speed */  & 0x07));
    if (!canon_message_send_with_reply (canon->fd, &msg, &reply)) {
      return FALSE;
    }

    canon_message_reset (&msg);
    canon_message_reset (&reply);
    canon_message_append (&msg, CANON_COMMAND);
    canon_message_append (&msg, CANON_CATEGORY_CAMERA1);
    canon_message_append (&msg, CANON_ZOOM);
    canon_message_append (&msg, CANON_ZOOM_TELE);
  } else if (0 < z) {
    canon_message_append (&msg,
        CANON_ZOOM_WIDE_SPEED | ((int) speed /*canon->zoom_speed */  & 0x07));
    if (!canon_message_send_with_reply (canon->fd, &msg, &reply)) {
      return FALSE;
    }

    canon_message_reset (&msg);
    canon_message_reset (&reply);
    canon_message_append (&msg, CANON_COMMAND);
    canon_message_append (&msg, CANON_CATEGORY_CAMERA1);
    canon_message_append (&msg, CANON_ZOOM);
    canon_message_append (&msg, CANON_ZOOM_WIDE);
  }

  return canon_message_send_with_reply (canon->fd, &msg, &reply);
}

static void
gst_cam_controller_canon_class_init (GstCamControllerCanonClass * canonclass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (canonclass);
  GstCamControllerClass *camctl_class = GST_CAM_CONTROLLER_CLASS (canonclass);
  object_class->finalize = GST_DEBUG_FUNCPTR (
      (GObjectFinalizeFunc) gst_cam_controller_canon_finalize);
  camctl_class->open = (GstCamControllerOpenFunc) gst_cam_controller_canon_open;
  camctl_class->close =
      (GstCamControllerCloseFunc) gst_cam_controller_canon_close;
  camctl_class->pan = (GstCamControllerPanFunc) gst_cam_controller_canon_pan;
  camctl_class->tilt = (GstCamControllerTiltFunc) gst_cam_controller_canon_tilt;
  camctl_class->move = (GstCamControllerMoveFunc) gst_cam_controller_canon_move;
  camctl_class->zoom = (GstCamControllerZoomFunc) gst_cam_controller_canon_zoom;
}
