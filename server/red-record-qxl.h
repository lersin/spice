/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RED_ABI_RECORD_H
#define RED_ABI_RECORD_H

#include <spice/qxl_dev.h>

#include "red-common.h"
#include "memslot.h"
#include "utils.h"

typedef struct RedRecord RedRecord;

/**
 * Create a new structure to handle recording.
 * This function never returns NULL.
 */
RedRecord* red_record_new(const char *filename);

void red_record_free(RedRecord *record);

void red_record_primary_surface_create(RedRecord *record,
                                       QXLDevSurfaceCreate *surface,
                                       uint8_t *line_0);

void red_record_event(RedRecord *record, int what, uint32_t type);

void red_record_qxl_command(RedRecord *record, RedMemSlotInfo *slots,
                            QXLCommandExt ext_cmd);

#endif
