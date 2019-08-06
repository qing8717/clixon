/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand

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
  
 * Restconf method implementation for operations get and data get and head
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/wait.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include <fcgiapp.h> /* Need to be after clixon_xml-h due to attribute format */

#include "restconf_lib.h"
#include "restconf_methods_get.h"

/*! Generic GET (both HEAD and GET)
 * According to restconf 
 * @param[in]  h      Clixon handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element 
 * @param[in]  pi     Offset, where path starts  
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  head   If 1 is HEAD, otherwise GET
 * @code
 *  curl -G http://localhost/restconf/data/interfaces/interface=eth0
 * @endcode                                     
 * See RFC8040 Sec 4.2 and 4.3
 * XXX: cant find a way to use Accept request field to choose Content-Type  
 *      I would like to support both xml and json.           
 * Request may contain                                        
 *     Accept: application/yang.data+json,application/yang.data+xml   
 * Response contains one of:                           
 *     Content-Type: application/yang-data+xml    
 *     Content-Type: application/yang-data+json  
 * NOTE: If a retrieval request for a data resource representing a YANG leaf-
 * list or list object identifies more than one instance, and XML
 * encoding is used in the response, then an error response containing a
 * "400 Bad Request" status-line MUST be returned by the server.
 * Netconf: <get-config>, <get>                        
 */
static int
api_data_get2(clicon_handle h,
	      FCGX_Request *r,
	      cvec         *pcvec,
	      int           pi,
	      cvec         *qvec,
	      int           pretty,
	      restconf_media media_out,
	      int           head)
{
    int        retval = -1;
    cbuf      *cbpath = NULL;
    char      *xpath = NULL;
    cbuf      *cbx = NULL;
    yang_stmt *yspec;
    cxobj     *xret = NULL;
    cxobj     *xerr = NULL; /* malloced */
    cxobj     *xe = NULL;   /* not malloced */
    cxobj    **xvec = NULL;
    size_t     xlen;
    int        i;
    cxobj     *x;
    int        ret;
    char      *namespace = NULL;
    cvec      *nsc = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    yspec = clicon_dbspec_yang(h);
    if ((cbpath = cbuf_new()) == NULL)
        goto done;
    cprintf(cbpath, "/");
    /* We know "data" is element pi-1 */
    if ((ret = api_path2xpath_cvv(pcvec, pi, yspec, cbpath, &namespace)) < 0)
	goto done;
    if (ret == 0){
	if (netconf_operation_failed_xml(&xerr, "protocol", clicon_err_reason) < 0)
	    goto done;
	clicon_err_reset();
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    xpath = cbuf_get(cbpath);
    clicon_debug(1, "%s path:%s", __FUNCTION__, xpath);
    /* Create a namespace context for ymod as the default namespace to use with
     * xpath expressions */
    if ((nsc = xml_nsctx_init(NULL, namespace)) == NULL)
	goto done;
    if (clicon_rpc_get(h, xpath, namespace, &xret) < 0){
	if (netconf_operation_failed_xml(&xerr, "protocol", clicon_err_reason) < 0)
	    goto done;
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if (xml_apply(xret, CX_ELMNT, xml_spec_populate, yspec) < 0)
	goto done;
    /* We get return via netconf which is complete tree from root 
     * We need to cut that tree to only the object.
     */
#if 0 /* DEBUG */
    if (debug){
	cbuf *cb = cbuf_new();
	clicon_xml2cbuf(cb, xret, 0, 0);
	clicon_debug(1, "%s xret:%s", __FUNCTION__, cbuf_get(cb));
	cbuf_free(cb);
    }
#endif
    /* Check if error return  */
    if ((xe = xpath_first(xret, "//rpc-error")) != NULL){
	if (api_return_err(h, r, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Normal return, no error */
    if ((cbx = cbuf_new()) == NULL)
	goto done;
    if (head){
	FCGX_SetExitStatus(200, r->out); /* OK */
	FCGX_FPrintF(r->out, "Content-Type: %s\r\n", restconf_media_int2str(media_out));
	FCGX_FPrintF(r->out, "\r\n");
	goto ok;
    }
    if (xpath==NULL || strcmp(xpath,"/")==0){ /* Special case: data root */
	switch (media_out){
	case YANG_DATA_XML:
	    if (clicon_xml2cbuf(cbx, xret, 0, pretty) < 0) /* Dont print top object?  */
		goto done;
	    break;
	case YANG_DATA_JSON:
	    if (xml2json_cbuf(cbx, xret, pretty) < 0)
		goto done;
	    break;
	}
    }
    else{
	if (xpath_vec_nsc(xret, nsc, "%s", &xvec, &xlen, xpath) < 0){
	    if (netconf_operation_failed_xml(&xerr, "application", clicon_err_reason) < 0)
		goto done;
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
	/* Check if not exists */
	if (xlen == 0){
	    /* 4.3: If a retrieval request for a data resource represents an 
	       instance that does not exist, then an error response containing 
	       a "404 Not Found" status-line MUST be returned by the server.  
	       The error-tag value "invalid-value" is used in this case. */
	    if (netconf_invalid_value_xml(&xerr, "application", "Instance does not exist") < 0)
		goto done;
	    /* override invalid-value default 400 with 404 */
	    if (api_return_err(h, r, xerr, pretty, media_out, 404) < 0)
		goto done;
	    goto ok;
	}
	switch (media_out){
	case YANG_DATA_XML:
	    for (i=0; i<xlen; i++){
		char *prefix, *namespace2; /* Same as namespace? */
		x = xvec[i];
		/* Some complexities in grafting namespace in existing trees to new */
		prefix = xml_prefix(x);
		if (xml_find_type_value(x, prefix, "xmlns", CX_ATTR) == NULL){
		    if (xml2ns(x, prefix, &namespace2) < 0)
			goto done;
		    if (namespace2 && xmlns_set(x, prefix, namespace2) < 0)
			goto done;
		}
		if (clicon_xml2cbuf(cbx, x, 0, pretty) < 0) /* Dont print top object?  */
		    goto done;
	    }
	    break;
	case YANG_DATA_JSON:
	    /* In: <x xmlns="urn:example:clixon">0</x>
	     * Out: {"example:x": {"0"}}
	     */
	    if (xml2json_cbuf_vec(cbx, xvec, xlen, pretty) < 0)
		goto done;
	    break;
	}
    }
    clicon_debug(1, "%s cbuf:%s", __FUNCTION__, cbuf_get(cbx));
    FCGX_SetExitStatus(200, r->out); /* OK */
    FCGX_FPrintF(r->out, "Cache-Control: no-cache\r\n");
    FCGX_FPrintF(r->out, "Content-Type: %s\r\n", restconf_media_int2str(media_out));
    FCGX_FPrintF(r->out, "\r\n");
    FCGX_FPrintF(r->out, "%s", cbx?cbuf_get(cbx):"");
    FCGX_FPrintF(r->out, "\r\n\r\n");
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (nsc)
	xml_nsctx_free(nsc);
    if (cbx)
        cbuf_free(cbx);
    if (cbpath)
	cbuf_free(cbpath);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (xvec)
	free(xvec);
    return retval;
}

/*! REST HEAD method
 * @param[in]  h      Clixon handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element 
 * @param[in]  pi     Offset, where path starts  
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 *
 * The HEAD method is sent by the client to retrieve just the header fields 
 * that would be returned for the comparable GET method, without the 
 * response message-body. 
 * Relation to netconf: none                        
 */
int
api_data_head(clicon_handle h,
	      FCGX_Request *r,
	      cvec         *pcvec,
	      int           pi,
	      cvec         *qvec,
	      int           pretty,
	      restconf_media media_out)
{
    return api_data_get2(h, r, pcvec, pi, qvec, pretty, media_out, 1);
}

/*! REST GET method
 * According to restconf 
 * @param[in]  h      Clixon handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element 
 * @param[in]  pi     Offset, where path starts  
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @code
 *  curl -G http://localhost/restconf/data/interfaces/interface=eth0
 * @endcode                                     
 * XXX: cant find a way to use Accept request field to choose Content-Type  
 *      I would like to support both xml and json.           
 * Request may contain                                        
 *     Accept: application/yang.data+json,application/yang.data+xml   
 * Response contains one of:                           
 *     Content-Type: application/yang-data+xml    
 *     Content-Type: application/yang-data+json  
 * NOTE: If a retrieval request for a data resource representing a YANG leaf-
 * list or list object identifies more than one instance, and XML
 * encoding is used in the response, then an error response containing a
 * "400 Bad Request" status-line MUST be returned by the server.
 * Netconf: <get-config>, <get>                        
 */
int
api_data_get(clicon_handle h,
	     FCGX_Request *r,
             cvec         *pcvec,
             int           pi,
             cvec         *qvec,
	     int           pretty,
	     restconf_media media_out)
{
    return api_data_get2(h, r, pcvec, pi, qvec, pretty, media_out, 0);
}

/*! GET restconf/operations resource
 * @param[in]  h      Clixon handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  path   According to restconf (Sec 3.5.1.1 in [draft])
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element 
 * @param[in]  pi     Offset, where path starts  
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  data   Stream input data
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 *
 * @code
 *  curl -G http://localhost/restconf/operations
 * @endcode                                     
 * RFC8040 Sec 3.3.2:
 * This optional resource is a container that provides access to the
 * data-model-specific RPC operations supported by the server.  The
 * server MAY omit this resource if no data-model-specific RPC
 * operations are advertised.
 * From ietf-restconf.yang:
 * In XML, the YANG module namespace identifies the module:
 *      <system-restart xmlns='urn:ietf:params:xml:ns:yang:ietf-system'/>
 * In JSON, the YANG module name identifies the module:
 *       { 'ietf-system:system-restart' : [null] }
 */
int
api_operations_get(clicon_handle h,
		   FCGX_Request *r, 
		   char         *path, 
		   cvec         *pcvec, 
		   int           pi,
		   cvec         *qvec, 
		   char         *data,
		   int           pretty,
		   restconf_media media_out)
{
    int        retval = -1;
    yang_stmt *yspec;
    yang_stmt *ymod; /* yang module */
    yang_stmt *yc;
    char      *namespace;
    cbuf      *cbx = NULL;
    cxobj     *xt = NULL;
    int        i;
    
    clicon_debug(1, "%s", __FUNCTION__);
    yspec = clicon_dbspec_yang(h);
    if ((cbx = cbuf_new()) == NULL)
	goto done;
    switch (media_out){
    case YANG_DATA_XML:
	cprintf(cbx, "<operations>");
	break;
    case YANG_DATA_JSON:
	cprintf(cbx, "{\"operations\": {");
	break;
    }
    ymod = NULL;
    i = 0;
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
	namespace = yang_find_mynamespace(ymod);
	yc = NULL; 
	while ((yc = yn_each(ymod, yc)) != NULL) {
	    if (yang_keyword_get(yc) != Y_RPC)
		continue;
	    switch (media_out){
	    case YANG_DATA_XML:
		cprintf(cbx, "<%s xmlns=\"%s\"/>", yang_argument_get(yc), namespace);
		break;
	    case YANG_DATA_JSON:
		if (i++)
		    cprintf(cbx, ",");
		cprintf(cbx, "\"%s:%s\": null", yang_argument_get(ymod), yang_argument_get(yc));
		break;
	    }
	}
    }
    switch (media_out){
    case YANG_DATA_XML:
	cprintf(cbx, "</operations>");
	break;
    case YANG_DATA_JSON:
	cprintf(cbx, "}}");
	break;
    }
    FCGX_SetExitStatus(200, r->out); /* OK */
    FCGX_FPrintF(r->out, "Content-Type: %s\r\n", restconf_media_int2str(media_out));
    FCGX_FPrintF(r->out, "\r\n");
    FCGX_FPrintF(r->out, "%s", cbx?cbuf_get(cbx):"");
    FCGX_FPrintF(r->out, "\r\n\r\n");
    // ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (cbx)
        cbuf_free(cbx);
    if (xt)
	xml_free(xt);
    return retval;
}

