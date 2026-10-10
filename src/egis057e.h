/*
 * Egis Technology EH57E / 057E driver for libfprint
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Hardware: LighTuning Technology Inc. EgisTec EH57E
 * USB ID:   1c7a:057e
 *
 * This header centralises every hardware constant, the image geometry and the
 * recovered ET5XX command packet tables. The implementation lives in
 * egis057e.c.
 *
 * Endpoint layout (confirmed from descriptors):
 *   bulk OUT      0x01
 *   bulk IN       0x82
 *   interrupt IN  0x83
 *   interrupt IN  0x84
 */

#ifndef EGIS057E_H
#define EGIS057E_H 1

#include "drivers_api.h"

/* -------------------------------------------------------------------------
 * USB endpoints and timeouts
 * ------------------------------------------------------------------------- */

#define EGIS057E_EP_OUT        0x01
#define EGIS057E_EP_IN         0x82
#define EGIS057E_EP_INT_FINGER 0x83
#define EGIS057E_EP_INT_ALT    0x84

#define EGIS057E_TIMEOUT_CMD   5000
#define EGIS057E_TIMEOUT_INT   1000
#define EGIS057E_RESP_LEN      7

/* -------------------------------------------------------------------------
 * Image geometry and command framing
 *
 * The exact live/calibration transfer is 0x0f96 = 3990 bytes, which is a
 * 70 x 57 grayscale frame.
 * ------------------------------------------------------------------------- */

#define EGIS057E_IMAGE_LEN     3990
#define EGIS057E_IMAGE_WIDTH   70
#define EGIS057E_IMAGE_HEIGHT  57

#define EGIS057E_PKT_LEN       7
#define EGIS057E_MAX_PKT_LEN   32

/* -------------------------------------------------------------------------
 * Command packet tables
 *
 * Recovered from EgisTouchFP057E.dll and validated against an EH57E.
 * Buffered-write responses (opcode 0x63 / 0x71) echo the request payload and
 * are therefore as long as the request.
 * ------------------------------------------------------------------------- */

typedef struct
{
  guint8       bytes[EGIS057E_MAX_PKT_LEN];
  guint        len;
  const char  *name;
  gboolean     expect_response;
} Egis057ePacket;

#define EGIS057E_INIT_PKT_COUNT     23
#define EGIS057E_CAPTURE_PKT_COUNT  2

extern const Egis057ePacket egis057e_init_pkts[EGIS057E_INIT_PKT_COUNT];
extern const Egis057ePacket egis057e_capture_pkts[EGIS057E_CAPTURE_PKT_COUNT];

#endif /* EGIS057E_H */
