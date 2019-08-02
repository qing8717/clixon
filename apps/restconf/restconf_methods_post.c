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
  
  * Restconf method implementation for post: operation(rpc) and data
  * From RFC 8040 Section 4.4.  POST
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

#include <fcgiapp.h> /* Need to be after clixon_xml.h due to attribute format */

#include "restconf_lib.h"
#include "restconf_methods_post.h"

/*! Generic REST POST  method 
 * @param[in]  h      CLIXON handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  api_path According to restconf (Sec 3.5.3.1 in rfc8040)
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element
 * @param[in]  pi     Offset, where to start pcvec
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  data   Stream input data
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  use_xml Set to 0 for JSON and 1 for XML for output data
 * @param[in]  parse_xml Set to 0 for JSON and 1 for XML for input data

 * restconf POST is mapped to edit-config create. 
 * @see RFC8040 Sec 4.4.1

 POST:
   target resource type is datastore --> create a top-level resource
   target resource type is  data resource --> create child resource

   The message-body MUST contain exactly one instance of the
   expected data resource.  The data model for the child tree is the
   subtree, as defined by YANG for the child resource.

   If the POST method succeeds, a "201 Created" status-line is returned
   and there is no response message-body.  A "Location" header
   identifying the child resource that was created MUST be present in
   the response in this case.

   If the data resource already exists, then the POST request MUST fail
   and a "409 Conflict" status-line MUST be returned.

 * @see RFC8040 Section 4.4
 * @see api_data_put
 */
int
api_data_post(clicon_handle h,
	      FCGX_Request *r, 
	      char         *api_path, 
	      cvec         *pcvec, 
	      int           pi,
	      cvec         *qvec, 
	      char         *data,
	      int           pretty,
	      int           use_xml,
    	      int           parse_xml)
{
    int        retval = -1;
    enum operation_type op = OP_CREATE;
    cxobj     *xdata0 = NULL; /* Original -d data struct (including top symbol) */
    cxobj     *xdata;         /* -d data (without top symbol)*/
    int        i;
    cbuf      *cbx = NULL;
    cxobj     *xtop = NULL; /* top of api-path */
    cxobj     *xbot = NULL; /* bottom of api-path */
    yang_stmt *ybot = NULL; /* yang of xbot */
    yang_stmt *ymodapi = NULL; /* yang module of api-path (if any) */
    yang_stmt *ymoddata = NULL; /* yang module of data (-d) */
    yang_stmt *yspec;
    cxobj     *xa;
    cxobj     *xret = NULL;
    cxobj     *xretcom = NULL; /* return from commit */
    cxobj     *xretdis = NULL; /* return from discard-changes */
    cxobj     *xerr = NULL; /* malloced must be freed */
    cxobj     *xe;            /* dont free */
    char      *username;
    int        nullspec = 0;
    int        ret;
    
    clicon_debug(1, "%s api_path:\"%s\"", __FUNCTION__, api_path);
    clicon_debug(1, "%s data:\"%s\"", __FUNCTION__, data);
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    for (i=0; i<pi; i++)
	api_path = index(api_path+1, '/');
    /* Create config top-of-tree */
    if ((xtop = xml_new("config", NULL, NULL)) == NULL)
	goto done;
    /* Translate api_path to xtop/xbot */
    xbot = xtop;
    if (api_path){
	if ((ret = api_path2xml(api_path, yspec, xtop, YC_DATANODE, 1, &xbot, &ybot)) < 0)
	    goto done;
	if (ybot)
	    ymodapi=ys_module(ybot);
	if (ret == 0){ /* validation failed */
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    clicon_err_reset();
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xtop, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s XURI:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif
    /* Parse input data as json or xml into xml */
    if (parse_xml){
	if (xml_parse_string(data, NULL, &xdata0) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    else {
	/* Data here cannot cannot (always) be Yang populated since it is
	 * loosely hanging without top symbols.
	 * And if it is not yang populated, it cant be translated properly
	 * from JSON to XML.
	 * Therefore, yang population is done later after addsub below
	 * Further complication is that if data is root resource, then it will
	 * work, so I need to check below that it didnt.
	 * THIS could be simplified.
	 */
	if ((ret = json_parse_str(data, yspec, &xdata0, &xerr)) < 0){ 
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
	if (ret == 0){
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    /* 4.4.1: The message-body MUST contain exactly one instance of the
     * expected data resource. 
     */
    if (xml_child_nr(xdata0) != 1){
	if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
	    goto done;
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    xdata = xml_child_i(xdata0,0);

    /* If the api-path (above) defines a module, then xdata must have a prefix
     * and it match the module defined in api-path. 
     * In a POST, maybe there are cornercases where xdata (which is a child) and
     * xbot (which is the parent) may have non-matching namespaces?
     * This does not apply if api-path is / (no module)
     */
    if (ys_module_by_xml(yspec, xdata, &ymoddata) < 0)
	goto done;
    if (ymoddata && ymodapi){
	if (ymoddata != ymodapi){
	     if (netconf_malformed_message_xml(&xerr, "Data is not prefixed with matching namespace") < 0)
		 goto done;
	     if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }

    /* Add operation (create/replace) as attribute */
    if ((xa = xml_new("operation", xdata, NULL)) == NULL)
	goto done;
    xml_type_set(xa, CX_ATTR);
    xml_prefix_set(xa, NETCONF_BASE_PREFIX);
    if (xml_value_set(xa, xml_operation2str(op)) < 0)
	goto done;
    /* Replace xbot with x, ie bottom of api-path with data */
    if (xml_addsub(xbot, xdata) < 0)
	goto done;
    /* xbot is already populated, resolve yang for added xdata too 
     */
    nullspec = (xml_spec(xdata) == NULL);
    if (xml_apply0(xdata, CX_ELMNT, xml_spec_populate, yspec) < 0)
	goto done;
    if (!parse_xml && nullspec){
	/* json2xml decode may not have been done above in json_parse,
	   need to be done here instead 
	   UNLESS it is a root resource, then json-parse has already done it
	*/
	if ((ret = json2xml_decode(xdata, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if (api_return_err(h, r, xerr, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }

    /* If restconf insert/point attributes are present, translate to netconf */
    if (restconf_insert_attributes(xdata, qvec) < 0)
	goto done;
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xdata, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s XDATA:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif

    /* Create text buffer for transfer to backend */
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    /* For internal XML protocol: add username attribute for access control
     */
    username = clicon_username_get(h);
    cprintf(cbx, "<rpc username=\"%s\" xmlns:%s=\"%s\">",
	    username?username:"",
	    NETCONF_BASE_PREFIX,
	    NETCONF_BASE_NAMESPACE); /* bind nc to netconf namespace */
    cprintf(cbx, "<edit-config><target><candidate /></target>");
    cprintf(cbx, "<default-operation>none</default-operation>");
    if (clicon_xml2cbuf(cbx, xtop, 0, 0) < 0)
	goto done;
    cprintf(cbx, "</edit-config></rpc>");
    clicon_debug(1, "%s xml: %s api_path:%s",__FUNCTION__, cbuf_get(cbx), api_path);
    if (clicon_rpc_netconf(h, cbuf_get(cbx), &xret, NULL) < 0)
	goto done;
    if ((xe = xpath_first(xret, "//rpc-error")) != NULL){
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Assume this is validation failed since commit includes validate */
    cbuf_reset(cbx);
    /* commit/discard should be done automaticaly by the system, therefore
     * recovery user is used here (edit-config but not commit may be permitted
     by NACM */
    cprintf(cbx, "<rpc username=\"%s\">", NACM_RECOVERY_USER);
    cprintf(cbx, "<commit/></rpc>");
    if (clicon_rpc_netconf(h, cbuf_get(cbx), &xretcom, NULL) < 0)
	goto done;
    if ((xe = xpath_first(xretcom, "//rpc-error")) != NULL){
	cbuf_reset(cbx);
	cprintf(cbx, "<rpc username=\"%s\">", username?username:"");
	cprintf(cbx, "<discard-changes/></rpc>");
	if (clicon_rpc_netconf(h, cbuf_get(cbx), &xretdis, NULL) < 0)
	    goto done;
	/* log errors from discard, but ignore */
	if ((xpath_first(xretdis, "//rpc-error")) != NULL)
	    clicon_log(LOG_WARNING, "%s: discard-changes failed which may lead candidate in an inconsistent state", __FUNCTION__);
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0) /* Use original xe */
	    goto done;
	goto ok;
    }
    if (xretcom){ /* Clear: can be reused again below */
	xml_free(xretcom);
	xretcom = NULL;
    }
    if (if_feature(yspec, "ietf-netconf", "startup")){
	/* RFC8040 Sec 1.4:
	 * If the NETCONF server supports :startup, the RESTCONF server MUST
	 * automatically update the non-volatile startup configuration
	 * datastore, after the "running" datastore has been altered as a
	 * consequence of a RESTCONF edit operation.
	 */
	cbuf_reset(cbx);
	cprintf(cbx, "<rpc username=\"%s\">", NACM_RECOVERY_USER);
	cprintf(cbx, "<copy-config><source><running/></source><target><startup/></target></copy-config></rpc>");
	if (clicon_rpc_netconf(h, cbuf_get(cbx), &xretcom, NULL) < 0)
	    goto done;
	/* If copy-config failed, log and ignore (already committed) */
	if ((xe = xpath_first(xretcom, "//rpc-error")) != NULL){

	    clicon_log(LOG_WARNING, "%s: copy-config running->startup failed", __FUNCTION__);
	}
    }
    FCGX_SetExitStatus(201, r->out);
    FCGX_FPrintF(r->out, "Status: 201 Created\r\n");
    http_location(r, xdata);
    FCGX_GetParam("HTTP_ACCEPT", r->envp);
    FCGX_FPrintF(r->out, "\r\n");
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (xretcom)
	xml_free(xretcom);
    if (xretdis)
	xml_free(xretdis);
    if (xtop)
	xml_free(xtop);
    if (xdata0)
	xml_free(xdata0);
     if (cbx)
	cbuf_free(cbx); 
   return retval;
} /* api_data_post */

/*! Handle input data to api_operations_post 
 * @param[in]  h      CLIXON handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  data   Stream input data
 * @param[in]  yspec  Yang top-level specification 
 * @param[in]  yrpc   Yang rpc spec
 * @param[in]  xrpc   XML pointer to rpc method
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  use_xml Set to 0 for JSON and 1 for XML for output data
 * @param[in]  parse_xml Set to 0 for JSON and 1 for XML for input data
 * @retval     1      OK
 * @retval     0      Fail, Error message sent
 * @retval    -1      Fatal error, clicon_err called
 *
 * RFC8040 3.6.1
 *  If the "rpc" or "action" statement has an "input" section, then
 *  instances of these input parameters are encoded in the module
 *  namespace where the "rpc" or "action" statement is defined, in an XML
 *  element or JSON object named "input", which is in the module
 *  namespace where the "rpc" or "action" statement is defined.
 * (Any other input is assumed as error.)
 */
static int
api_operations_post_input(clicon_handle h,
			  FCGX_Request *r, 
			  char         *data,
			  yang_stmt    *yspec,
			  yang_stmt    *yrpc,
			  cxobj        *xrpc,
			  int           pretty,
			  int           use_xml,
			  int           parse_xml)
{
    int        retval = -1;
    cxobj     *xdata = NULL;
    cxobj     *xerr = NULL; /* malloced must be freed */
    cxobj     *xe;
    cxobj     *xinput;
    cxobj     *x;
    cbuf      *cbret = NULL;
    int        ret;

    clicon_debug(1, "%s %s", __FUNCTION__, data);
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    /* Parse input data as json or xml into xml */
    if (parse_xml){
	if (xml_parse_string(data, yspec, &xdata) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto fail;
	}
    }
    else { /* JSON */
	if ((ret = json_parse_str(data, yspec, &xdata, &xerr)) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto fail;
	}
	if (ret == 0){
	    if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto fail;
	}
    }
    xml_name_set(xdata, "data");
    /* Here xdata is: 
     * <data><input xmlns="urn:example:clixon">...</input></data>
     */
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xdata, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s DATA:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif
    /* Validate that exactly only <input> tag */
    if ((xinput = xml_child_i_type(xdata, 0, CX_ELMNT)) == NULL ||
	strcmp(xml_name(xinput),"input") != 0 ||
	xml_child_nr_type(xdata, CX_ELMNT) != 1){

	if (xml_child_nr_type(xdata, CX_ELMNT) == 0){
	    if (netconf_malformed_message_xml(&xerr, "restconf RPC does not have input statement") < 0)
		goto done;
	}
	else
	    if (netconf_malformed_message_xml(&xerr, "restconf RPC has malformed input statement (multiple or not called input)") < 0)
		goto done;	
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto fail;
    }
    //    clicon_debug(1, "%s input validation passed", __FUNCTION__);
    /* Add all input under <rpc>path */
    x = NULL;
    while ((x = xml_child_i_type(xinput, 0, CX_ELMNT)) != NULL)
	if (xml_addsub(xrpc, x) < 0) 	
	    goto done;
    /* Here xrpc is:  <myfn xmlns="uri"><x>42</x></myfn>
     */
    // ok:
    retval = 1;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (cbret)
	cbuf_free(cbret);
    if (xerr)
	xml_free(xerr);
    if (xdata)
	xml_free(xdata);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Handle output data to api_operations_post 
 * @param[in]  h        CLIXON handle
 * @param[in]  r        Fastcgi request handle
 * @param[in]  xret     XML reply messages from backend/handler
 * @param[in]  yspec    Yang top-level specification 
 * @param[in]  youtput  Yang rpc output specification
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  use_xml Set to 0 for JSON and 1 for XML for output data
 * @param[out] xoutputp Restconf JSON/XML output
 * @retval     1        OK
 * @retval     0        Fail, Error message sent
 * @retval    -1        Fatal error, clicon_err called
 * xret should like: <top><rpc-reply><x xmlns="uri">0</x></rpc-reply></top>
 */
static int
api_operations_post_output(clicon_handle h,
			   FCGX_Request *r, 
			   cxobj        *xret,
			   yang_stmt    *yspec,
			   yang_stmt    *youtput,
			   char         *namespace,
			   int           pretty,
			   int           use_xml,
			   cxobj       **xoutputp)
    
{
    int        retval = -1;
    cxobj     *xoutput = NULL;
    cxobj     *xerr = NULL; /* assumed malloced, will be freed */
    cxobj     *xe;          /* just pointer */
    cxobj     *xa;          /* xml attribute (xmlns) */
    cxobj     *x;
    cxobj     *xok;
    int        isempty;
    
    //    clicon_debug(1, "%s", __FUNCTION__);
    /* Validate that exactly only <rpc-reply> tag */
    if ((xoutput = xml_child_i_type(xret, 0, CX_ELMNT)) == NULL ||
	strcmp(xml_name(xoutput),"rpc-reply") != 0 ||
	xml_child_nr_type(xret, CX_ELMNT) != 1){
	if (netconf_malformed_message_xml(&xerr, "restconf RPC does not have single input") < 0)
	    goto done;	
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto fail;
    }
    /* xoutput should now look: <rpc-reply><x xmlns="uri">0</x></rpc-reply> */
    /* 9. Translate to restconf RPC data */
    xml_name_set(xoutput, "output");
    /* xoutput should now look: <output><x xmlns="uri">0</x></output> */
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xoutput, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s XOUTPUT:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif

    /* Sanity check of outgoing XML 
     * For now, skip outgoing checks.
     * (1) Does not handle <ok/> properly
     * (2) Uncertain how validation errors should be logged/handled
     */
    if (youtput!=NULL){
	xml_spec_set(xoutput, youtput); /* needed for xml_spec_populate */
#if 0
	if (xml_apply(xoutput, CX_ELMNT, xml_spec_populate, yspec) < 0)
	    goto done;
	if ((ret = xml_yang_validate_all(xoutput, &xerr)) < 0)
	    goto done;
	if (ret == 1 &&
	    (ret = xml_yang_validate_add(h, xoutput, &xerr)) < 0)
	    goto done;
	if (ret == 0){ /* validation failed */
	    if ((xe = xpath_first(xerr, "rpc-reply/rpc-error")) == NULL){
		clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
		goto done;
	    }
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto fail;
	}
#endif
    }
    /* Special case, no yang output (single <ok/> - or empty body?)
     * RFC 7950 7.14.4
     * If the RPC operation invocation succeeded and no output parameters
     * are returned, the <rpc-reply> contains a single <ok/> element
     * RFC 8040 3.6.2
     * If the "rpc" statement has no "output" section, the response message
     * MUST NOT include a message-body and MUST send a "204 No Content"
     * status-line instead.
     */
    isempty = xml_child_nr_type(xoutput, CX_ELMNT) == 0 ||
	(xml_child_nr_type(xoutput, CX_ELMNT) == 1 &&
	 (xok = xml_child_i_type(xoutput, 0, CX_ELMNT)) != NULL &&
	 strcmp(xml_name(xok),"ok")==0);
    if (isempty) {
	/* Internal error - invalid output from rpc handler */
	FCGX_SetExitStatus(204, r->out); /* OK */
	FCGX_FPrintF(r->out, "Status: 204 No Content\r\n");
	FCGX_FPrintF(r->out, "\r\n");
	goto fail;
    }
    /* Clear namespace of parameters */
    x = NULL;
    while ((x = xml_child_each(xoutput, x, CX_ELMNT)) != NULL) {
	if ((xa = xml_find_type(x, NULL, "xmlns", CX_ATTR)) != NULL)
	    if (xml_purge(xa) < 0)
		goto done;
    }
    /* Set namespace on output */
    if (xmlns_set(xoutput, NULL, namespace) < 0)
	goto done;
    *xoutputp = xoutput;
    retval = 1;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (xerr)
	xml_free(xerr);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! REST operation POST method 
 * @param[in]  h      CLIXON handle
 * @param[in]  r      Fastcgi request handle
 * @param[in]  path   According to restconf (Sec 3.5.1.1 in [draft])
 * @param[in]  pcvec  Vector of path ie DOCUMENT_URI element
 * @param[in]  pi     Offset, where to start pcvec
 * @param[in]  qvec   Vector of query string (QUERY_STRING)
 * @param[in]  data   Stream input data
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  use_xml Set to 0 for JSON and 1 for XML for output data
 * @param[in]  parse_xml Set to 0 for JSON and 1 for XML for input data
 * See RFC 8040 Sec 3.6 / 4.4.2
 * @note We map post to edit-config create. 
 *      POST {+restconf}/operations/<operation>
 * 1. Initialize
 * 2. Get rpc module and name from uri (oppath) and find yang spec
 * 3. Build xml tree with user and rpc: <rpc username="foo"><myfn xmlns="uri"/>
 * 4. Parse input data (arguments):
 *             JSON: {"example:input":{"x":0}}
 *             XML:  <input xmlns="uri"><x>0</x></input>
 * 5. Translate input args to Netconf RPC, add to xml tree:
 *             <rpc username="foo"><myfn xmlns="uri"><x>42</x></myfn></rpc>
 * 6. Validate outgoing RPC and fill in default values
 *  <rpc username="foo"><myfn xmlns="uri"><x>42</x><y>99</y></myfn></rpc>
 * 7. Send to RPC handler, either local or backend
 * 8. Receive reply from local/backend handler as Netconf RPC
 *       <rpc-reply><x xmlns="uri">0</x></rpc-reply>
 * 9. Translate to restconf RPC data:
 *             JSON: {"example:output":{"x":0}}
 *             XML:  <output xmlns="uri"><x>0</x></input>
 * 10. Validate and send reply to originator
 */
int
api_operations_post(clicon_handle h,
		    FCGX_Request *r, 
		    char         *path, 
		    cvec         *pcvec, 
		    int           pi,
		    cvec         *qvec, 
		    char         *data,
		    int           pretty,
		    int           use_xml,
		    int           parse_xml)
{
    int        retval = -1;
    int        i;
    char      *oppath = path;
    yang_stmt *yspec;
    yang_stmt *youtput = NULL;
    yang_stmt *yrpc = NULL;
    cxobj     *xret = NULL;
    cxobj     *xerr = NULL; /* malloced must be freed */
    cxobj     *xtop = NULL; /* xpath root */
    cxobj     *xbot = NULL;
    yang_stmt *y = NULL;
    cxobj     *xoutput = NULL;
    cxobj     *xa;
    cxobj     *xe;
    char      *username;
    cbuf      *cbret = NULL;
    int        ret = 0;
    char      *prefix = NULL;
    char      *id = NULL;
    yang_stmt *ys = NULL;
    char      *namespace = NULL;
    
    clicon_debug(1, "%s json:\"%s\" path:\"%s\"", __FUNCTION__, data, path);
    /* 1. Initialize */
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    for (i=0; i<pi; i++)
	oppath = index(oppath+1, '/');
    if (oppath == NULL || strcmp(oppath,"/")==0){
	if (netconf_operation_failed_xml(&xerr, "protocol", "Operation name expected") < 0)
	    goto done;
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    /* 2. Get rpc module and name from uri (oppath) and find yang spec 
     *       POST {+restconf}/operations/<operation>
     *
     * The <operation> field identifies the module name and rpc identifier
     * string for the desired operation.
     */
    if (nodeid_split(oppath+1, &prefix, &id) < 0) /* +1 skip / */
	goto done;
    if ((ys = yang_find(yspec, Y_MODULE, prefix)) == NULL){
	if (netconf_operation_failed_xml(&xerr, "protocol", "yang module not found") < 0)
	    goto done;
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    if ((yrpc = yang_find(ys, Y_RPC, id)) == NULL){
	if (netconf_missing_element_xml(&xerr, "application", id, "RPC not defined") < 0)
	    goto done;
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    /* 3. Build xml tree with user and rpc: 
     * <rpc username="foo"><myfn xmlns="uri"/>
     */
    if ((xtop = xml_new("rpc", NULL, NULL)) == NULL)
	goto done;
    xbot = xtop;
    /* Here xtop is: <rpc/> */
    if ((username = clicon_username_get(h)) != NULL){
	if ((xa = xml_new("username", xtop, NULL)) == NULL)
	    goto done;
	xml_type_set(xa, CX_ATTR);
	if (xml_value_set(xa, username) < 0)
	    goto done;
	/* Here xtop is: <rpc username="foo"/> */
    }
    if ((ret = api_path2xml(oppath, yspec, xtop, YC_SCHEMANODE, 1, &xbot, &y)) < 0)
	goto done;
    if (ret == 0){ /* validation failed */
	if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
	    goto done;
	clicon_err_reset();
	if ((xe = xpath_first(xerr, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto done;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Here xtop is: <rpc username="foo"><myfn xmlns="uri"/></rpc> 
     * xbot is <myfn xmlns="uri"/>
     * 4. Parse input data (arguments):
     *             JSON: {"example:input":{"x":0}}
     *             XML:  <input xmlns="uri"><x>0</x></input>
     */
    namespace = xml_find_type_value(xbot, NULL, "xmlns", CX_ATTR);
    clicon_debug(1, "%s : 4. Parse input data: %s", __FUNCTION__, data);
    if (data && strlen(data)){
	if ((ret = api_operations_post_input(h, r, data, yspec, yrpc, xbot,
					     pretty, use_xml, parse_xml)) < 0)
	    goto done;
	if (ret == 0)
	    goto ok;
    }
    /* Here xtop is: 
      <rpc username="foo"><myfn xmlns="uri"><x>42</x></myfn></rpc> */
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xtop, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s 5. Translate input args: %s",
		     __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif
    /* 6. Validate incoming RPC and fill in defaults */
    if (xml_spec_populate_rpc(h, xtop, yspec) < 0) /*  */
	goto done;
    if ((ret = xml_yang_validate_rpc(h, xtop, &xret)) < 0)
	goto done;
    if (ret == 0){
	if ((xe = xpath_first(xret, "rpc-error")) == NULL){
	    clicon_err(OE_XML, EINVAL, "rpc-error not found (internal error)");
	    goto ok;
	}
	if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Here xtop is (default values):
     * <rpc username="foo"><myfn xmlns="uri"><x>42</x><y>99</y></myfn></rpc>
    */
#if 0
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xtop, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s 6. Validate and defaults:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif
    /* 7. Send to RPC handler, either local or backend
     * Note (1) xtop is <rpc><method> xbot is <method>
     *      (2) local handler wants <method> and backend wants <rpc><method>
     */
    /* Look for local (client-side) restconf plugins. 
     * -1:Error, 0:OK local, 1:OK backend 
     */
    if ((ret = rpc_callback_call(h, xbot, cbret, r)) < 0)
	goto done;
    if (ret > 0){ /* Handled locally */
	if (xml_parse_string(cbuf_get(cbret), NULL, &xret) < 0)
	    goto done;
	/* Local error: return it and quit */
	if ((xe = xpath_first(xret, "rpc-reply/rpc-error")) != NULL){
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    else {    /* Send to backend */
	if (clicon_rpc_netconf_xml(h, xtop, &xret, NULL) < 0)
	    goto done;
	if ((xe = xpath_first(xret, "rpc-reply/rpc-error")) != NULL){
	    if (api_return_err(h, r, xe, pretty, use_xml, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    /* 8. Receive reply from local/backend handler as Netconf RPC
     *       <rpc-reply><x xmlns="uri">0</x></rpc-reply>
     */
#if 1
    if (debug){
	cbuf *ccc=cbuf_new();
	if (clicon_xml2cbuf(ccc, xret, 0, 0) < 0)
	    goto done;
	clicon_debug(1, "%s 8. Receive reply:%s", __FUNCTION__, cbuf_get(ccc));
	cbuf_free(ccc);
    }
#endif
    youtput = yang_find(yrpc, Y_OUTPUT, NULL);
    if ((ret = api_operations_post_output(h, r, xret, yspec, youtput, namespace,
					  pretty, use_xml, &xoutput)) < 0)
	goto done;
    if (ret == 0)
	goto ok;
    /* xoutput should now look: <output xmlns="uri"><x>0</x></output> */
    FCGX_SetExitStatus(200, r->out); /* OK */
    FCGX_FPrintF(r->out, "Content-Type: application/yang-data+%s\r\n", use_xml?"xml":"json");
    FCGX_FPrintF(r->out, "\r\n");
    cbuf_reset(cbret);
    if (use_xml){
	if (clicon_xml2cbuf(cbret, xoutput, 0, pretty) < 0)
	    goto done;
	/* xoutput should now look: <output xmlns="uri"><x>0</x></output> */
    }
    else{
	if (xml2json_cbuf(cbret, xoutput, pretty) < 0)
	    goto done;
	/* xoutput should now look: {"example:output": {"x":0,"y":42}} */
    }
    FCGX_FPrintF(r->out, "%s", cbuf_get(cbret));
    FCGX_FPrintF(r->out, "\r\n\r\n");
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (prefix)
	free(prefix);
    if (id)
	free(id);
    if (xtop)
	xml_free(xtop);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (cbret)
	cbuf_free(cbret);
   return retval;
}
