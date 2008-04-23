/*
 * Seahorse
 *
 * Copyright (C) 2006 Stefan Walter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __SEAHORSE_TOOL_H__
#define __SEAHORSE_TOOL_H__

#include "seahorse-pgp-operation.h"

struct _SeahorseToolMode;
typedef gboolean (*SeahorseToolCallback) (struct _SeahorseToolMode *mode, const gchar *uri,
                                          gpgme_data_t uridata, SeahorsePGPOperation *pop,
                                          GError **err);

typedef struct _SeahorseToolMode {

    const gchar *title;
    const gchar *errmsg;
    gboolean package;

    /* Used for encryption /signing */
    gpgme_key_t *recipients;
    gpgme_key_t signer;

    /* Used for import */
    guint imports;

    /* Callbacks for various functions */
    SeahorseToolCallback startcb;
    SeahorseToolCallback donecb;
    gpointer userdata;

} SeahorseToolMode;

int  seahorse_tool_files_process  (SeahorseToolMode *mode, const gchar **uris);

/* -----------------------------------------------------------------------------
 * PROGRESS FUNCTIONS
 */

int         seahorse_tool_progress_init    (int argc, char* argv[]);

int         seahorse_tool_progress_start   (const gchar *title);

gboolean    seahorse_tool_progress_check   ();

void        seahorse_tool_progress_block   (gboolean block);

gboolean    seahorse_tool_progress_update  (gdouble fract, const gchar *message);

void        seahorse_tool_progress_stop    ();

#endif /* __SEAHORSE_TOOL_H__ */
