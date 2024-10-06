/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2023 Olof Hagsand

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

 * RFC 8528 Yang schema mount support
 *
 * Extend a container with ietf-yang-schema-mount:mount-point.
 * Structure of mount-points in YANG anc config:
 *
 * module ietf-yang-schema-mount { # Existing module
 *   extension mount-point
 *
 * module mymodule { # Your module
 *   ...
 *   container root{ (ymnt)
 *     yangmnt:mount-point "mylabel";  (yext)
 *   }
 * (note the argument "mylabel" defines an optional isolated YANG domain
 *
 * <config>         # Your XML config
 *   ...
 *   <root>         (xmnt)
 *
 * The API handles the relation between yext -->* ymnt -->* xmnt
 * Structure:
 *
 *   yspec0(1)     xtop(1)
 *   |             | (xpath)
 *   ymnt(*)  <--  xmnt(*)
 *  /     \
 * yext(1) cvec: [xpath = yspec](*)
 * |                      |
 * cv:label               ymod(*)
 *
 * The calls in this code are:
 * - yang_schema_mount_point(): Is ymnt a yang mount-point? (ymnt)
 * - yang_mount_get(): ymnt + xpath -> yspec
 * - yang_mount_get2(): ymnt + xpath -> yspec # NEW
 * - yang_mount_set(): ymnt + xpath -> yspec
 * - xml_yang_mount_get(): xmnt-> yspec
 * - xml_yang_mount_set(): xmnt -> yspec
 * - yang_mount_get_yspec_any(): ymnt -> yspec
 * - yang_mounto_freeall(): ymnt-> free cvec
 * - yang_mount_xmnt2ymnt_xpath(): xmnt -> ymnt + xpath
 * - yang_mount_xtop2xmnt(): top-level xml -> xmnt vector
 * - yang_mount_yspec2ymnt(): top-level yspec -> ymnt vector
 * - yang_schema_mount_statistics(): Given xtop -> find all xmnt -> stats

 *
 * Note: the xpath used as key in yang unknown cvec is "canonical" in the sense:
 * - it uses prefixes of the yang spec of relevance
 * - it uses '' not "" in prefixes (eg a[x='foo']. The reason is '' is easier printed in clispecs
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_map.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_options.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_io.h"
#include "clixon_xml_map.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_module.h"
#include "clixon_yang_parse_lib.h"
#include "clixon_plugin.h"
#include "clixon_xml_bind.h"
#include "clixon_xml_nsctx.h"
#include "clixon_yang_schema_mount.h"

/*! Check if YANG node is a RFC 8528 YANG schema mount
 *
 * Check if:
 * - y is CONTAINER or LIST, AND
 * - y has YANG schema mount "mount-point" as child element, AND
 * - the extension label matches y (see note below)
 * If so, then return 1
 * @param[in] y   Yang statement
 * @retval    1   Yes, y is a RFC 8528 YANG mount-point
 * @retval    0   No, y is not
 * @retval   -1   Error
 * @note That this may be a restriction on the usage of "label". The RFC is somewhat unclear.
 */
int
yang_schema_mount_point0(yang_stmt *y)
{
    int           retval = -1;
    enum rfc_6020 keyw;
    int           exist = 0;
    char         *value = NULL;

    if (y == NULL){
        clixon_err(OE_YANG, EINVAL, "y is NULL");
        goto done;
    }
    keyw = yang_keyword_get(y);
    if (keyw != Y_CONTAINER
#ifndef YANG_SCHEMA_MOUNT_ONLY_PRESENCE_CONTAINERS
        && keyw != Y_LIST
#endif
#if 0 /* See this in some standard YANGs but RFC 8528 does not allow it */
        && keyw != Y_ANYDATA
#endif
        )
        goto fail;
    if (yang_extension_value(y, "mount-point", YANG_SCHEMA_MOUNT_NAMESPACE, &exist, &value) < 0)
        goto done;
    if (exist == 0)
        goto fail;
    if (value == NULL)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Cached variant of yang_schema_mount_point0
 *
 * @param[in]  y   Yang node
 * @retval     1   Yes, node is potential mountpoint
 * @retval     0   No, node is not potential mountpoint
 */
int
yang_schema_mount_point(yang_stmt *y)
{
    return yang_flag_get(y, YANG_FLAG_MTPOINT_POTENTIAL) ? 1 : 0;
}

/*! Get yangspec mount-point
 *
 * @param[in]  y     Yang container/list containing unknown node
 * @param[in]  xpath Key for yspec on y
 * @param[out] yspec YANG stmt spec
 * @retval     0     OK
 * @retval    -1     Error
 */
int
yang_mount_get(yang_stmt  *ys,
               char       *xpath,
               yang_stmt **yspecp)
{
    int        retval = 1;
    yang_stmt *ymounts;
    yang_stmt *ydomain;
    yang_stmt *yspec = NULL;
    int        inext;
    int        inext2;

    if ((ymounts = ys_mounts(ys)) == NULL){
        clixon_err(OE_YANG, ENOENT, "Top-level yang mounts not found");
        goto done;
    }
    inext = 0;
    ydomain = NULL;
    while ((ydomain = yn_iter(ymounts, &inext)) != NULL) {
        inext2 = 0;
        while ((yspec = yn_iter(ydomain, &inext2)) != NULL) {
            if (yang_keyword_get(yspec) != Y_SPEC ||
                yang_cvec_get(yspec) == NULL ||
                yang_flag_get(yspec, YANG_FLAG_SPEC_MOUNT) == 0)
                continue;
            if (xpath == NULL || cvec_find(yang_cvec_get(yspec), xpath) != NULL)
                break;
        }
        if (yspec != NULL)
            break;
    }
    *yspecp = yspec;
    retval = 0;
 done:
    return retval;
}

/*! Get any yspec of a mount-point, special function
 *
 * Get (the first) mounted yspec.
 * A more generic way would be to call plugin_mount to get the yanglib and from that get the
 * yspec. But there is clixon code that cant call the plugin since h is not available
 * @param[in]  ys    Yang container/list containing unknown node
 * @param[out] yspec YANG stmt spec
 * @retval     1     yspec found and set
 * @retval     0     Not found
 * @retval    -1     Error
 * XXX Should be in-lined
 */
int
yang_mount_get_yspec_any(yang_stmt  *ys,
                         yang_stmt **yspecp)
{
    yang_stmt *yspec = NULL;

    if (yang_mount_get(ys, NULL, &yspec) < 0)
        return -1;
    if (yspec == NULL)
        return 0;
    *yspecp = yspec;
    return 1;
}

/*! Set yangspec mount-point on yang node containing extension
 *
 * Mount-points are stored in yang cvec in container/list node that is a mount-point
 * as defined in yang_schema_mount_point()
 * @param[in]  y      Yang container/list containing unknown node
 * @param[in]  xpath  Key for yspec on y, in canonical form
 * @param[in]  yspec  Yangspec for this mount-point (consumed)
 * @retval     0      OK
 * @retval    -1      Error
 */
int
yang_mount_set(yang_stmt *y,
               char      *xpath,
               yang_stmt *yspec)
{
    int        retval = -1;

    yang_flag_set(y, YANG_FLAG_MOUNTPOINT); /* Cache value */
    retval = 0;
    return retval;
}

/*! Given an XML mount-point return YANG mount and XPath
 *
 * @param[in]  h     Clixon handle
 * @param[in]  xmnt  XML mount-point
 * @param[out] ymnt  YANG mount-point
 * @param[out] xpath Canonical XPath from XML top-level to xmnt, free after use
 * @retval     1     OK, xmnt is a mount-point with ymnt and xpath returned
 * @retval     0     OK, xmnt is not a mount point
 * @retval    -1     Error
 */
static int
yang_mount_xmnt2ymnt_xpath(clixon_handle h,
                           cxobj        *xmnt,
                           yang_stmt   **ymntp,
                           char        **xpath)
{
    int        retval = -1;
    yang_stmt *yspec;
    yang_stmt *ymnt;
    char      *xpath0 = NULL;
    cvec      *nsc0 = NULL;
    cvec      *nsc1 = NULL;
    cbuf      *reason = NULL;
    int        ret;

    if (xmnt == NULL){
        clixon_err(OE_YANG, EINVAL, "xmnt is NULL");
        goto done;
    }
    if ((ymnt = xml_spec(xmnt)) == NULL)
        goto fail;
    if (yang_schema_mount_point(ymnt) == 0)
        goto fail;
    if (xml2xpath(xmnt, NULL, 1, 0, &xpath0) < 0)
        goto done;
    if (xml_nsctx_node(xmnt, &nsc0) < 0)
        goto done;
    yspec = clicon_dbspec_yang(h);
    if ((ret = xpath2canonical(xpath0, nsc0, yspec, xpath, &nsc1, &reason)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (ymntp)
        *ymntp = ymnt;
    retval = 1;
 done:
    if (xpath0)
        free(xpath0);
    if (nsc0)
        cvec_free(nsc0);
    if (nsc1)
        cvec_free(nsc1);
    if (reason)
        cbuf_free(reason);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given XML mount-point get yangspec mount-point
 *
 * @param[in]  h        Clixon handle
 * @param[in]  xmnt     XML mount-point
 * @param[out] vallevel Do or dont do full RFC 7950 validation if given
 * @param[out] xpathp   malloced xpath in canonical form (if ret is 1)
 * @param[out] yspec    YANG stmt spec of mount-point (if ret is 1)
 * @retval     1        x is a mount-point: yspec may be set
 * @retval     0        x is not a mount-point
 * @retval    -1        Error
 */
int
xml_yang_mount_get(clixon_handle   h,
                   cxobj          *xmnt,
                   validate_level *vl,
                   char          **xpathp,
                   yang_stmt     **yspec)
{
    int        retval = -1;
    yang_stmt *ymnt = NULL;
    char      *xpath = NULL;
    int        ret;

    if ((ret = yang_mount_xmnt2ymnt_xpath(h, xmnt, &ymnt, &xpath)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Check validate level */
    if (vl && clixon_plugin_yang_mount_all(h, xmnt, NULL, vl, NULL) < 0)
        goto done;
    if (yspec && yang_mount_get(ymnt, xpath, yspec) < 0)
        goto done;
    if (xpathp){
        *xpathp = xpath;
        xpath = NULL;
    }
    retval = 1;
 done:
    if (xpath)
        free(xpath);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Set yangspec mount-point via XML mount-point node
 *
 * Stored in a separate structure (not in XML config tree)
 * @param[in]  h      Clixon handle
 * @param[in]  xmnt   XML mount-point
 * @param[in]  yspec  Yangspec for this mount-point (consumed)
 * @retval     0      OK
 * @retval    -1      Error
 */
int
xml_yang_mount_set(clixon_handle h,
                   cxobj        *xmnt,
                   yang_stmt    *yspec)
{
    int        retval = -1;
    yang_stmt *ymnt = NULL;
    char      *xpath = NULL;
    int        ret;

    if ((ret = yang_mount_xmnt2ymnt_xpath(h, xmnt, &ymnt, &xpath)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_YANG, 0, "Mapping xmnt to ymnt and xpath");
        goto done;
    }
    if (yang_mount_set(ymnt, xpath, yspec) < 0)
        goto done;
    retval = 0;
 done:
    if (xpath)
        free(xpath);
    return retval;
}

/*! Find schema mounts - callback function for xml_apply
 *
 * @param[in]  x    XML node  
 * @param[in]  arg  cvec, if match add node
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
find_xml_schema_mounts(cxobj *x,
                       void  *arg)
{
    int        ret;
    yang_stmt *y;
    cvec      *cvv = (cvec *)arg;
    cg_var    *cv;

    if ((y = xml_spec(x)) == NULL)
        return 2;
    if (yang_config(y) == 0)
        return 2;
    if ((ret = yang_schema_mount_point(y)) < 0)
        return -1;
    if (ret == 0)
        return 0;
    if ((cv = cvec_add(cvv, CGV_VOID)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_add");
        return -1;
    }
    cv_void_set(cv, x);
    return 0;
}

/*! Given XML top-of-tree, find all XML mount-points and return in a vector
 *
 * @param[in]  h     Clixon handle
 * @param[in]  xtop  XML top-node
 * @param[out] cvv   Cligen vector of XML moint-points, deallocate with cvec_fee()
 * @retval     0     OK
 * @retval    -1     Error
 */
int
yang_mount_xtop2xmnt(cxobj *xtop,
                     cvec **cvvp)
{
    int    retval = -1;
    cvec  *cvv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xml_apply(xtop, CX_ELMNT, find_xml_schema_mounts, cvv) < 0)
        goto done;
    if (cvvp)
        *cvvp = cvv;
    retval = 0;
 done:
    return retval;
}

/*! Find schema mounts - callback function for yang_apply
 *
 * @param[in]  yn   yang node
 * @param[in]  arg  Argument
 * @retval     n    OK, abort traversal and return to caller with "n"
 * @retval     0    OK, continue with next
 * @retval    -1    Error, abort
 */
static int
find_yang_schema_mounts(yang_stmt *y,
                        void      *arg)
{
    int     ret;
    cvec   *cvv = (cvec *)arg;
    cg_var *cv;

    if (yang_config(y) == 0)
        return 0;
    if ((ret = yang_schema_mount_point(y)) < 0)
        return -1;
    if (ret == 0)
        return 0;
    if ((cv = cvec_add(cvv, CGV_VOID)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_add");
        return -1;
    }
    cv_void_set(cv, y);
    return 0;
}

/*! Given top-level YANG spec, find all YANG mount-points and return in a vector
 *
 * @param[in]  h      Clixon handle
 * @param[in]  yspec  YANG top-level spec
 * @param[out] cvv    Cligen vector of YANG moint-points, free with cvec_fee()
 * @retval     0      OK
 * @retval    -1      Error
 */
int
yang_mount_yspec2ymnt(yang_stmt *yspec,
                      cvec     **cvvp)
{
    int    retval = -1;
    cvec  *cvv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (yang_apply(yspec, -1, find_yang_schema_mounts, 1, cvv) < 0)
        goto done;
    if (cvvp)
        *cvvp = cvv;
    retval = 0;
 done:
    return retval;
}

/*! Find mount-points and return yang-library state
 *
 * Brute force: traverse whole XML, match all x that have ymount as yspec
 * Add yang-library state for all x
 * @param[in]     h       Clixon handle
 * @param[in]     xpath   XML Xpath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @param[out]    xerr    XML error tree, if retval = 0
 * @retval        1       OK
 * @retval        0       Validation failed, error in xret
 * @retval       -1       Error (fatal)
 *
 * RFC 8528 Section 3.4:
 *   A schema for a mount point contained in a mounted module can be
 *   specified by implementing the "ietf-yang-library" and
 *   "ietf-yang-schema-mount" modules in the mounted schema and specifying
 *   the schemas in exactly the same way as the top-level schema.
 * Alt: see snmp_yang2xml to get instances instead of brute force traverse of whole tree
 * XXX  Mountpoints must exist in xret on entry, which is problematic:
 * XXX  A get state may have an xpath not including their config, ie:
 * XXX  xpath=/top/mymount/yang-library does not include /top/mymount and therefore
 * XXX  the mountpoint will not be present in xret
 * XXX see: https://github.com/clicon/clixon/issues/485
 */
static int
yang_schema_mount_statedata_yanglib(clixon_handle h,
                                    char         *xpath,
                                    cvec         *nsc,
                                    cxobj       **xret,
                                    cxobj       **xerr)
{
    int            retval = -1;
    cvec          *cvv = NULL;
    cg_var        *cv;
    cxobj         *xmnt;
    cxobj         *yanglib = NULL; /* xml yang-lib */
    cbuf          *cb = NULL;
    yang_stmt     *yspec;
    int            ret;
    int            config = 1;
    validate_level vl = VL_FULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, 0, "clicon buffer");
        goto done;
    }
    if (yang_mount_xtop2xmnt(*xret, &cvv) < 0)
        goto done;
    yspec = clicon_dbspec_yang(h);
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
        xmnt = cv_void_get(cv);
        yanglib = NULL;
        /* User callback */
        if (clixon_plugin_yang_mount_all(h, xmnt, &config, &vl, &yanglib) < 0)
            goto done;
        if (yanglib == NULL)
            continue;
        if ((ret = xml_bind_yang0(h, yanglib, YB_MODULE, yspec, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        if (xml_addsub(xmnt, yanglib) < 0)
            goto done;
        yanglib = NULL;
    }
    retval = 1;
 done:
    if (cvv)
        cvec_free(cvv);
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get schema mount-point state according to RFC 8528
 *
 * @param[in]     h       Clixon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   XML XPath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @param[out]    xerr    XML error tree, if retval = 0
 * @retval        1       OK
 * @retval        0       Validation failed, error in xret
 * @retval       -1       Error (fatal)
 * @note  Only "inline" specification of mounted schema supported, not "shared schema"
 */
int
yang_schema_mount_statedata(clixon_handle h,
                            yang_stmt    *yspec,
                            char         *xpath,
                            cvec         *nsc,
                            cxobj       **xret,
                            cxobj       **xerr)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    int        ret;
    yang_stmt *yext;
    yang_stmt *ymount;
    yang_stmt *ymodext;
    yang_stmt *ymod;
    cg_var    *cv;
    cg_var    *cv1;
    char      *label;
    cvec      *cvv;
    cxobj     *x1 = NULL;

    if ((ymodext = yang_find(yspec, Y_MODULE, "ietf-yang-schema-mount")) == NULL ||
        (yext = yang_find(ymodext, Y_EXTENSION, "mount-point")) == NULL){
        goto ok;
        //        clixon_err(OE_YANG, 0, "yang schema mount-point extension not found");
        //        goto done;
    }
    if ((cvv = yang_cvec_get(yext)) != NULL){
        if ((cb = cbuf_new()) ==NULL){
            clixon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "<schema-mounts xmlns=\"%s\">", YANG_SCHEMA_MOUNT_NAMESPACE); // XXX only if hit
        cv = NULL;
        while ((cv = cvec_each(cvv, cv)) != NULL){
            ymount = (yang_stmt*)cv_void_get(cv);
            ymod = ys_module(ymount);
            if ((cv1 = yang_cv_get(ymount)) == NULL){
                clixon_err(OE_YANG, 0, "mount-point extension must have label");
                goto done;
            }
            label = cv_string_get(cv1);
            cprintf(cb, "<mount-point>");
            cprintf(cb, "<module>%s</module>", yang_argument_get(ymod));
            cprintf(cb, "<label>%s</label>", label);
            cprintf(cb, "<inline/>");
            cprintf(cb, "</mount-point>");
        }
        cprintf(cb, "</schema-mounts>");
        if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x1, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        if (xpath_first(x1, nsc, "%s", xpath) != NULL){
            if ((ret = netconf_trymerge(x1, yspec, xret)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
        }
    }
    /* Find mount-points and return yang-library state */
    if (yang_schema_mount_statedata_yanglib(h, xpath, nsc, xret, xerr) < 0)
        goto done;
 ok:
    retval = 1;
 done:
    if (x1)
        xml_free(x1);
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given xml mount-point and yanglib, find existing yspec
 *
 * Get and loop through all XML from xt mount-points.
 * Get xyanglib and if equal to xt, find and return yspec
 * @param[in]   h        Clixon handle
 * @param[in]   xt       XML tree node
 * @param[in]   xyanglib yanglib in XML
 * @param[out]  yspecp   Yang spec
 * @retval      0        OK
 * @retval     -1        Error
 */
static int
yang_schema_find_share(clixon_handle h,
                       cxobj        *xt,
                       cxobj        *xyanglib,
                       yang_stmt   **yspecp)
{
    int     retval = -1;
    cvec   *cvv = NULL;
    cg_var *cv;
    cxobj  *xroot;
    cxobj  *xmnt;
    cxobj  *xylib;
    int     config = 1;
    int     ret;

    xroot = xml_root(xt);
    /* Get all XML mtpoints */
    if (yang_mount_xtop2xmnt(xroot, &cvv) < 0)
        goto done;
    /* Loop through XML mount-points */
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
        xmnt = cv_void_get(cv);
        if (xmnt == xt)
            continue;
        xylib = NULL;
        /* Get xyanglib */
        if (clixon_plugin_yang_mount_all(h, xmnt, &config, NULL, &xylib) < 0)
            goto done;
        if (xylib == NULL)
            continue;
        /* Check if equal */
        if (xml_tree_equal(xyanglib, xylib) == 1)
            continue;
        /* Find and return yspec */
        *yspecp = NULL;
        if ((ret = xml_yang_mount_get(h, xmnt, NULL, NULL, yspecp)) < 0)
            goto done;
        if (ret == 1 && *yspecp != NULL)
            break;
    }
    retval = 0;
 done:
    if (cvv)
        cvec_free(cvv);
    return retval;
}

/*! Get yanglib from user plugin callback, parse it and mount it
 *
 * Optionally check for shared yspec
 * @param[in]  h   Clixon handle
 * @param[in]  xt  XML tree node
 * @retval     1   OK
 * @retval     0   No yanglib or problem when parsing yanglib
 * @retval    -1   Error
 */
int
yang_schema_yanglib_parse_mount(clixon_handle h,
                                cxobj        *xt)
{
    int        retval = -1;
    cxobj     *xyanglib = NULL;
    cxobj     *xb;
    yang_stmt *ymounts;
    yang_stmt *ydomain;
    yang_stmt *yspec0 = NULL;
    yang_stmt *yspec1 = NULL;
    char      *xpath = NULL;
    char      *domain = NULL;
    cbuf      *cb = NULL;
    int        ret;
    static unsigned int nr = 0;

    /* 1. Get modstate (xyanglib) of node: xyanglib, by querying backend state (via callback)
     *    XXX this xyanglib is not proper RFC8525, submodules appear as modules WHY?
     */
    if (clixon_plugin_yang_mount_all(h, xt, NULL, NULL, &xyanglib) < 0)
        goto done;
    if (xyanglib == NULL)
        goto anydata;
    if ((xb = xpath_first(xyanglib, NULL, "module-set/name")) != NULL)
        domain = xml_body(xb);
    if (domain == NULL){
        clixon_err(OE_YANG, 0, "domain not found");
        goto done;
    }
    /* Get xpath */
    if ((ret = yang_mount_xmnt2ymnt_xpath(h, xt, NULL, &xpath)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_YANG, 0, "Mapping xmnt to ymnt and xpath");
        goto done;
    }
    if ((ymounts = clixon_yang_mounts_get(h)) == NULL){
        clixon_err(OE_YANG, ENOENT, "Top-level yang mounts not found");
        goto done;
    }
    if ((ydomain = yang_find(ymounts, Y_DOMAIN, domain)) == NULL){
        if ((ydomain = ydomain_new(h, domain)) == NULL)
            goto done;
    }
    /* Optimization: find equal yspec from other mount-point */
    if (clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT_SHARE")) {
        if (yang_schema_find_share(h, xt, xyanglib, &yspec0) < 0)
            goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_YANG, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%u", nr++);
    if ((yspec1 = yspec_new_shared(h, xpath, domain, cbuf_get(cb), yspec0)) < 0)
        goto done;
    /* Either yspec0 = NULL and yspec1 is new, or yspec0 == yspec1 != NULL (shared) */
    if (yspec0 == NULL && yspec1 != NULL){
        if ((ret = yang_lib2yspec(h, xyanglib, xpath, domain, yspec1)) < 0)
            goto done;
        if (ret == 0){
            ys_prune_self(yspec1); /* remove from tree, free in done code */
            goto anydata;
        }
    }
    if (xml_yang_mount_set(h, xt, yspec1) < 0)
        goto done;
    yspec1 = NULL;
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    if (xpath)
        free(xpath);
    if (yspec1)
        ys_free(yspec1);
    if (xyanglib)
        xml_free(xyanglib);
    return retval;
 anydata:   // Treat as anydata
    retval = 0;
    goto done;
}

/*! Check if XML node is mount-point and return matching YANG child
 *
 * @param[in]     h       Clixon handle
 * @param[in]     x1      XML node
 * @param[in]     x1c     A child of x1
 * @param[out]    yc      YANG child
 * @retval        1       OK, yc contains child
 * @retval        0       No such child
 * @retval       -1       Error
 * XXX maybe not needed
 */
int
yang_schema_get_child(clixon_handle h,
                      cxobj        *x1,
                      cxobj        *x1c,
                      yang_stmt   **yc)
{
    int        retval = -1;
    yang_stmt *yspec1;
    yang_stmt *ymod1 = NULL;
    char      *x1cname;
    int        ret;

    x1cname = xml_name(x1c);
    if ((ret = xml_yang_mount_get(h, x1, NULL, NULL, &yspec1)) < 0)
        goto done;
    if (ret == 1 && yspec1 != NULL){
        if (ys_module_by_xml(yspec1, x1c, &ymod1) <0)
            goto done;
        if (ymod1 != NULL)
            *yc = yang_find_datanode(ymod1, x1cname);
        else{ /* It is in fact a mountpoint, there is a yang mount, but it is not found */
            goto fail;
        }
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Remove xpath from yspec cvec list, remove yspec if empty
 *
 * @param[in]  h    Clixon handle
 * @param[in]  xmnt XML mount-point
 */
int
yang_schema_yspec_rm(clixon_handle h,
                     cxobj        *xmnt)
{
    /* Remove mountpoint from yspec cvec */
    int        retval = -1;
    yang_stmt *yspec = NULL;
    char      *xpath = NULL;
    int        ret;

    if ((ret = xml_yang_mount_get(h, xmnt, NULL, &xpath, &yspec)) < 0)
        goto done;
    if (ret == 1 && xpath != NULL && yspec != NULL){
        if (yang_cvec_rm(yspec, xpath) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (xpath)
        free(xpath);
    return retval;
}
