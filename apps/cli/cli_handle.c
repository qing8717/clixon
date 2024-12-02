/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <grp.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_handle.h"

#define CLICON_MAGIC 0x99aafabe

#define handle(h) (assert(clixon_handle_check(h)==0),(struct cli_handle *)(h))
#define cligen(h) (handle(h)->cl_cligen)

/*! CLI specific handle added to header Clixon handle
 *
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 * @note The top part must be equivalent to struct clixon_handle in clixon_handle.c
 * @see struct clixon_handle, struct backend_handle
 */
struct cli_handle {
    int             cl_magic;    /* magic (HDR)*/
    clicon_hash_t  *cl_copt;     /* clicon option list (HDR) */
    clicon_hash_t  *cl_data;     /* internal clicon data (HDR) */
    clicon_hash_t  *ch_db_elmnt; /* xml datastore element cache data */
    event_stream_t *cl_stream;   /* notification streams, see clixon_stream.[ch] */
    /* ------ end of common handle ------ */

    cligen_handle   cl_cligen;   /* cligen handle */
};

/*! Return a clicon handle for other CLICON API calls
 */
clixon_handle
cli_handle_init(void)
{
    struct cli_handle *cl;
    cligen_handle      clih = NULL;
    clixon_handle      h = NULL;

    if ((cl = (struct cli_handle *)clixon_handle_init0(sizeof(struct cli_handle))) == NULL)
        return NULL;

    if ((clih = cligen_init()) == NULL){
        clixon_handle_exit((clixon_handle)cl);
        goto done;
    }
    cligen_userhandle_set(clih, cl);
    cligen_eval_wrap_fn_set(clih, clixon_resource_check, cl);
    cl->cl_cligen = clih;

    h = (clixon_handle)cl;
  done:
    return h;
}

/*! Free clicon handle
 *
 * @param[in] h      Clixon handle
 */
int
cli_handle_exit(clixon_handle h)
{
    cligen_handle      ch = cligen(h);

    clixon_handle_exit(h); /* frees h and options */
    cligen_exit(ch);
    return 0;
}

/*----------------------------------------------------------
 * cli-specific handle access functions
 *----------------------------------------------------------*/

/*! Return clicon handle 
 *
 * @param[in] h      Clixon handle
 */
cligen_handle
cli_cligen(clixon_handle h)
{
    return cligen(h);
}

int
cli_parse_file(clixon_handle h,
               FILE         *f,
               char         *name, /* just for errs */
               parse_tree   *pt,
               cvec         *globals)
{
    cligen_handle ch = cligen(h);

    return clispec_parse_file(ch, f, name, NULL, pt, globals);
}

int
cli_susp_hook(clixon_handle     h,
              cligen_susp_cb_t *fn)
{
    cligen_handle ch = cligen(h);

    /* This assume first arg of fn can be treated as void* */
    return cligen_susp_hook(ch, fn);
}

int
cli_interrupt_hook(clixon_handle          h,
                   cligen_interrupt_cb_t *fn)
{
    cligen_handle ch = cligen(h);

    /* This assume first arg of fn can be treated as void* */
    return cligen_interrupt_hook(ch, fn);
}

int
cli_prompt_set(clixon_handle h,
               char         *prompt)
{
    cligen_handle ch = cligen(h);
    return cligen_prompt_set(ch, prompt);
}

int
cli_logsyntax_set(clixon_handle h,
                  int           status)
{
    cligen_handle ch = cligen(h);
    return cligen_logsyntax_set(ch, status);
}
