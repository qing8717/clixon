/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Clixon XML XPATH 1.0 according to https://www.w3.org/TR/xpath-10
 * Note: for YANG which is constrained to path-arg as defined in rfc7950
 * See: clixon_xpath.[ch] for full XML XPATH implementation
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_nsctx.h"
#include "clixon_yang_module.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xpath_function.h"
#include "clixon_xpath_yang.h"

/* Assume single yang node context */
struct xp_yang_ctx{
    enum xp_objtype xy_type;
    yang_stmt      *xy_node;    /* If type is XT_NODESET */
    int             xy_bool;    /* If type is XT_BOOL */
    yang_stmt      *xy_initial; /* RFC 7960 10.1.1 extension: for current() */
};
typedef struct xp_yang_ctx xp_yang_ctx;

/* Forward */
static int xp_yang_eval(xp_yang_ctx *xy, xpath_tree *xptree, xp_yang_ctx **xyr);

/*! Duplicate xpath yang context */
xp_yang_ctx *
xy_dup(xp_yang_ctx *xy0)
{
    xp_yang_ctx *xy = NULL;
    
    if ((xy = malloc(sizeof(*xy))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xy, 0, sizeof(*xy));
    if (xy0)
        *xy = *xy0;
    else
        xy->xy_type = XT_NODESET;
 done:
    return xy;
}

/*! xpath yang equality operator returns true
 *
 * rfc7950 sec 9.9.2:
 * Predicates are used only for constraining the values for the
 * key nodes for list entries.  Each predicate consists of exactly one
 * equality test per key
 * Always evaluates to true since there are no instances 
 */
static int
xp_yang_op_eq(xp_yang_ctx *xy1,
             xp_yang_ctx  *xy2,
             xp_yang_ctx **xyr)
{
    int          retval = -1;
    xp_yang_ctx *xy = NULL;
    
    if ((xy = xy_dup(xy1)) == NULL)
        goto done;
    if (xy1 == NULL || xy2 == NULL || xy1->xy_node == NULL || xy2->xy_node == NULL){
        clicon_err(OE_YANG, EINVAL, "Invalid path-arg (Error in xy1 or xy2) ");
        goto done;
    }
    xy->xy_type = XT_BOOL;
    xy->xy_bool = 1;
    xy->xy_node = NULL;
    *xyr = xy;
    retval = 0;
 done:
    return retval;
}

/*! Evaluate leafref PATH-ARG step rule on a YANG tree
 *
 * @param[in]  xy0        Incoming context
 * @param[in]  xpath_tree XPATH parse-tree
 * @param[out] xyr        Resulting context
 * @retval     0          OK
 * @retval    -1          Error
 * @see xp_eval_step
 */
static int
xp_yang_eval_step(xp_yang_ctx  *xy0,
                  xpath_tree   *xptree,
                  xp_yang_ctx **xyr)
{
    int          retval = -1;
    xpath_tree  *nodetest; /* needed if child */
    char        *prefix;
    yang_stmt   *ys;
    xp_yang_ctx *xy = NULL;
    yang_stmt   *ys1 = NULL;

    /* Create new xy */
    if ((xy = xy_dup(xy0)) == NULL)
        goto done;
    ys = xy->xy_node;
    switch (xptree->xs_int){
    case A_CHILD: 
        if ((nodetest = xptree->xs_c0) == NULL){
            clicon_err(OE_YANG, 0, "child step nodetest expected");
            goto done;
        }
        switch (nodetest->xs_type){
        case XP_NODE:
            if ((prefix = nodetest->xs_s0) != NULL){
                /* XXX: Kludge with prefixes */
                if (yang_keyword_get(ys) == Y_SPEC){ /* This means top */
                    if ((ys1 = yang_find_module_by_prefix_yspec(ys, prefix)) != NULL)
                        ys = ys1;
                }
                else if (yang_keyword_get(ys) == Y_MODULE){ /* This means top */
                    if ((ys1 = yang_find_module_by_prefix(ys, prefix)) == NULL)
                        ys1 = yang_find_module_by_prefix_yspec(ys_spec(ys), prefix);
                    if (ys1 != NULL)
                        ys = ys1;
                }
            }
            xy->xy_node = yang_find_schemanode(ys, nodetest->xs_s1);
            if (xy->xy_node == NULL){
                *xyr = xy;
                xy = NULL;
                goto ok;
            }
            break;
        case XP_NODE_FN:
            break;
        default:
            clicon_err(OE_YANG, 0, "Invalid xpath-tree nodetest: %s",
                       xpath_tree_int2str(nodetest->xs_type));
            goto done;
            break;
        } /* nodetest xs_type */
        break;
    case A_PARENT:
        xy->xy_node = yang_parent_get(ys);
        break;
    default:
        clicon_err(OE_YANG, 0, "Invalid path-arg step: %s",
                   axis_type_int2str(xptree->xs_int));
        goto done;
        break;
    }
    if (xptree->xs_c1){
        if (xp_yang_eval(xy, xptree->xs_c1, xyr) < 0)
            goto done;
    }
    else{
        *xyr = xy;
        xy = NULL;
    }
 ok:
    retval = 0;
 done:
    if (xy)
        free(xy);
    return retval;
}

/*! Evaluate leafref PATH-ARG predicate rule on a YANG tree
 *
 * @param[in]  xy         Incoming context
 * @param[in]  xpath_tree XPATH parse-tree
 * @param[out] xyr        Resulting context
 * @retval     0          OK
 * @retval    -1          Error
 * @see xp_eval_predicate
 */
static int
xp_yang_eval_predicate(xp_yang_ctx  *xy,
                       xpath_tree   *xptree,
                       xp_yang_ctx **xyr)
{
    int         retval = -1;
    xp_yang_ctx *xy0 = NULL;
    xp_yang_ctx *xy1 = NULL;

    if (xptree->xs_c0 != NULL){ /* eval previous predicates */
        if (xp_yang_eval(xy, xptree->xs_c0, &xy0) < 0)  
            goto done;
    }
    else{  /* empty */
        if ((xy0 = xy_dup(xy)) == NULL)
            goto done;
    }
    if (xptree->xs_c1){ /* Second child */
        //      if ((xy1 = xy_dup(xy)) == NULL)
        //          goto done;
        /* the PredicateExpr is evaluated with the node as the context node */
        if (xp_yang_eval(xy0, xptree->xs_c1, &xy1) < 0)
            goto done;
        /* Check xrc: if "true" then xyr=xy0? */
        if (xy1->xy_type == XT_BOOL && xy1->xy_bool)
            ;
        else
            xy0->xy_node = NULL;
    }
    *xyr = xy0;
    xy0 = NULL;
    retval = 0;
 done:
    if (xy0)
        free(xy0);
    if (xy1)
        free(xy1);
   return retval;
}

/*! Evaluate leafref PATH-ARG on a YANG tree
 *
 * @param[in]  xy         Incoming context
 * @param[in]  xpath_tree XPATH parse-tree
 * @param[out] xyr        Resulting context
 * @retval     0          OK
 * @retval    -1          Error
 * @see xp_eval
 */
static int
xp_yang_eval(xp_yang_ctx  *xy,
             xpath_tree   *xptree,
             xp_yang_ctx **xyr)
{
    int          retval = -1;
    int          use_xy0 = 0;
    xp_yang_ctx *xy0 = NULL;
    xp_yang_ctx *xy1 = NULL;
    xp_yang_ctx *xy2 = NULL;

    /* If empty npodeset, quit, cannot continue */
    if (xy->xy_type == XT_NODESET && xy->xy_node == NULL)
        goto ok;
    /* Pre-actions before check first child c0
     */
    switch (xptree->xs_type){
    case XP_EXP:
    case XP_AND:
    case XP_ADD:
    case XP_UNION:
        if (xptree->xs_c1 != NULL){
            clicon_err(OE_XML, 0, "Function %s having two args is invalid for path-arg", xptree->xs_s0);
            goto done;
        }
        break;
    case XP_RELEX:
    case XP_PATHEXPR:
    case XP_FILTEREXPR:
        break;
    case XP_LOCPATH:
    case XP_NODE:
    case XP_NODE_FN:
        break;
    case XP_RELLOCPATH:
        break;
    case XP_PRIME_FN:
        if (xptree->xs_s0){
            switch (xptree->xs_int){
            case XPATHFN_CURRENT:
                if ((*xyr = xy_dup(xy)) == NULL)
                    goto done;
                (*xyr)->xy_node = (*xyr)->xy_initial;
                goto ok;
                break;
            default:
                clicon_err(OE_XML, 0, "Function %s invalid for path-arg", xptree->xs_s0);
                goto done;
            }
        }
        break;
    case XP_PRIME_STR:
        if ((*xyr = xy_dup(xy)) == NULL)
            goto done;
        goto ok;
        break;
    case XP_ABSPATH:
        /* Set context node to top node, and nodeset to that node only */
        if (yang_keyword_get(xy->xy_node) != Y_SPEC)
            xy->xy_node = ys_module(xy->xy_node);
        break;
    case XP_PRED:
        if (xp_yang_eval_predicate(xy, xptree, xyr) < 0)
            goto done;
        goto ok; /* Skip generic child traverse */
        break;
    case XP_STEP:    /* XP_NODE is first argument -not called explicitly */
        if (xp_yang_eval_step(xy, xptree, xyr) < 0)
            goto done;
        goto ok; /* Skip generic child traverse */
        break;
    default: /* Here we explicitly fail on node types for those not appearing in path-arg */
        clicon_err(OE_YANG, 0, "Invalid xpath-tree node name: %s",
                   xpath_tree_int2str(xptree->xs_type));
        goto done;
        break;
    }
    /* Eval first child c0
     */
    if (xptree->xs_c0){
        if (xp_yang_eval(xy, xptree->xs_c0, &xy0) < 0)  
            goto done;
    }
    /* Actions between first and second child
     */
    switch (xptree->xs_type){
    case XP_RELLOCPATH:
    case XP_ABSPATH:
        use_xy0++;
        break;
    case XP_PATHEXPR:
        if (xptree->xs_c1)
            use_xy0++;
        break;
    default:
        break;
    }
    /* Eval second child c1
     * Note, some operators like locationpath, need transitive context (use_xr0)
     */
    if (xptree->xs_c1){
        if (xp_yang_eval(use_xy0?xy0:xy, xptree->xs_c1, &xy1) < 0)      
            goto done;
        /* Actions after second child
         */
        switch (xptree->xs_type){
        case XP_RELEX: /* relexpr --> addexpr | relexpr relop addexpr */
            /* Check op: only EQ allowed in path-arg */
            if (xptree->xs_int != XO_EQ){
                clicon_err(OE_YANG, 0, "Invalid xpath-tree relational operator: %d, only eq allowed",
                           xptree->xs_int);
                goto done;
            }
            if (xp_yang_op_eq(xy0, xy1, &xy2) < 0)
                goto done;
            break;
        default:
            break;
        }
    }
    if (xy0 == NULL && xy1 == NULL && xy2 == NULL){
        if (xptree->xs_type == XP_ABSPATH){
            if ((*xyr = xy_dup(xy)) == NULL)
                goto done;
        }
        else {
            clicon_err(OE_XML, EFAULT, "Internal error: no result produced");
            goto done;
        }
    }
    if (xy2){
        *xyr = xy2;
        xy2 = NULL;
    }
    else if (xy1){
        *xyr = xy1;
        xy1 = NULL;
    }
    else if (xy0){
        *xyr = xy0;
        xy0 = NULL;
    }
 ok:
    retval = 0;
 done:
    if (xy2)
        free(xy2);
    if (xy1)
        free(xy1);
    if (xy0)
        free(xy0);
    return retval;
}

/*! Resolve a yang node given a start yang node and a leafref path-arg
 *
 * Leafrefs have a path arguments that are used both for finding referred XML node instances as well
 * as finding a referred YANG node for typechecks.
 * Such a path-arg is defined as:
 *   The syntax for a path argument is a subset of the XPath abbreviated
 *   syntax.  Predicates are used only for constraining the values for the
 *   key nodes for list entries.  Each predicate consists of exactly one
 *   equality test per key, and multiple adjacent predicates MAY be
 *   present if a list has multiple keys.
 * @param[in]   ys        YANG referring node
 * @param[in]   path_arg  path-arg
 * @param[out]  yref      YANG referred node
 * @note this function uses XPATH parser, which is (much too) general
 * @code
 *   yang_stmt    *ys;              // source / referring node
 *   yang_stmt    *yref = NULL;     // target / referred node
 *   char         *path_arg="../config/name";
 *
 *   if (yang_path_arg(ys, path_arg, &yref) < 0)
 *     err;
 * @endcode

 * @see rfc7950 Sec 9.9.2 
 * @see rfc7950 Sec 14 (leafref path)
 */
int
yang_path_arg(yang_stmt  *ys,
              const char *path_arg,
              yang_stmt **yref)
{
    int          retval = -1;
    xpath_tree  *xptree = NULL;
    xp_yang_ctx *xyr = NULL;
    xp_yang_ctx *xy = NULL;

    clicon_debug(2, "%s", __FUNCTION__);
    if (path_arg == NULL){
        clicon_err(OE_XML, EINVAL, "path-arg is NULL");
        goto done;
    }
    if (xpath_parse(path_arg, &xptree) < 0)
        goto done;
    if ((xy = xy_dup(NULL)) == NULL)
        goto done;
    xy->xy_node = ys;
    xy->xy_initial = ys;
    if (xp_yang_eval(xy, xptree, &xyr) < 0)
        goto done;
    if (xyr != NULL)
        *yref = xyr->xy_node;
    retval = 0;
 done:
    if (xptree)
        xpath_tree_free(xptree);
    if (xyr)
        free(xyr);
    if (xy)
        free(xy);
    return retval;
}
