/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Kristofer Hallin
  Sponsored by Siklu Communications LTD

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
  * This is the clixon_snmp daemon
  * It assumes a netsnmp damon is running. 
  * - If netsnmp does not run, clixon_snmp will not start
  * - If netsnmp dies, clixon_snmp will exit
  * - If netsnmp is restarted, clixon_snmp  should also be restarted
  * It is possible to be more resilient, such as setting a timer and trying again, in fact, libnetsnmp
  * has some such mechanisms but these are NOT implemented
  * @see RFC 6643 Translation of Structure of Management Information Version 2 (SMIv2)
  *               MIB Modules to YANG Modules
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "snmp_lib.h"
#include "snmp_register.h"
#include "snmp_stream.h"

/* Command line options to be passed to getopt(3) */
#define SNMP_OPTS "hVD:f:l:C:o:z"

/* Forward */
static int clixon_snmp_input_cb(int s, void *arg);

/*! Return (hardcoded) pid file
 */
static char*
clicon_snmp_pidfile(clixon_handle h)
{
    return "/var/tmp/clixon_snmp.pid";
}

/*! Signal terminates process
 *
 * Just set exit flag for proper exit in event loop
 */
static void
clixon_snmp_sig_term(int arg)
{
    clixon_log(NULL, LOG_NOTICE, "%s: %s: pid: %u Signal %d",
               __PROGRAM__, __FUNCTION__, getpid(), arg);
    /* This should ensure no more accepts or incoming packets are processed because next time eventloop
     * is entered, it will terminate.
     * However there may be a case of sockets closing rather abruptly for clients
     */
    clixon_exit_set(1);
}

/*! Clean and close all state of netconf process (but dont exit).
 *
 * Cannot use h after this
 * @param[in]  h  Clixon handle
 */
static int
snmp_terminate(clixon_handle h)
{
    cvec      *nsctx;
    cxobj     *x = NULL;
    char      *pidfile = clicon_snmp_pidfile(h);

    clixon_snmp_stream_shutdown(h);
    snmp_shutdown(__FUNCTION__);
    shutdown_agent();
    clixon_snmp_api_agent_cleanup();
    if (clicon_ptr_get(h, "snmp-rowstatus-tree", (void**)&x) == 0 && x){
        xml_free(x);
        x = NULL;
    }
    clicon_rpc_close_session(h);
    yang_exit(h);
    if ((nsctx = clicon_nsctx_global_get(h)) != NULL)
        cvec_free(nsctx);
    if ((x = clicon_conf_xml(h)) != NULL)
        xml_free(x);
    xpath_optimize_exit();
    clixon_event_exit();
    clixon_handle_exit(h);
    clixon_err_exit();
    clixon_log_exit();
    if (pidfile)
        unlink(pidfile);
    return 0;
}

/*! Get which sockets are used from SNMP API, the register single sockets into clixon event system
 *
 * @param[in]  h        Clixon handle
 * @param[in]  register If 1 register snmp sockets with event handler. If 0 close and unregister
 * @retval     0        OK
 * @retval    -1        Error
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * if you use select(), see snmp_select_info() in snmp_api(3)
 * snmp_select_info(int *numfds, fd_set *fdset, struct timeval *timeout, int *block)
 * @see clixon_snmp_input_cb
 */
static int
clixon_snmp_fdset_register(clixon_handle h,
                           int           regfd)
{
    int             retval = -1;
    int             numfds = 0;
    fd_set          readfds;
    struct timeval  timeout = { LONG_MAX, 0 };
    int             block = 0;
    int             nr;
    int             s;

    FD_ZERO(&readfds);
    if ((nr = snmp_sess_select_info(NULL, &numfds, &readfds, &timeout, &block)) < 0){
        clixon_err(OE_XML, errno, "snmp_select_error");
        goto done;
    }
    /* eg 4, 6, 8 */
    for (s=0; s<numfds; s++){
        if (FD_ISSET(s, &readfds)){
            clixon_debug(CLIXON_DBG_SNMP, "%d", s);
            if (regfd){
                if (clixon_event_reg_fd(s, clixon_snmp_input_cb, h, "snmp socket") < 0)
                    goto done;
            }
            else{
                if (clixon_event_unreg_fd(s, clixon_snmp_input_cb) < 0)
                    goto done;
                close(s);
            }
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Callback for single socket
 *
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * @param[in]  s   Read socket
 * @param[in]  arg Clixon handle
 * @retval     0   OK
 * @retval    -1   Error
 */
static int
clixon_snmp_input_cb(int   s,
                     void *arg)
{
    int            retval = -1;
    fd_set         readfds;
    clixon_handle  h = (clixon_handle)arg;
    int            ret;

    clixon_debug(CLIXON_DBG_SNMP | CLIXON_DBG_DETAIL, "%d", s);
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    (void)snmp_read(&readfds);
    if (clixon_event_poll(s) < 0){
        if (errno == EBADF){
            clixon_err_reset();
            /* Close the active socket */
            if (clixon_event_unreg_fd(s, clixon_snmp_input_cb) < 0)
                goto done;
            close(s);
            /* and then the others */
            if (clixon_snmp_fdset_register(h, 0) < 0)
                goto done;
            if ((ret = snmp_close_sessions()) != 1){
                clixon_err(OE_SNMP, ret, "snmp_close_sessions");
                goto done;
            }
            /* Signal normal exit to upper layers (=event handling)
             * One can signal error and return -1, but it is nicer with an orderly exit
             */
            clixon_exit_set(1);
        }
        else {
            clixon_err(OE_UNIX, errno, "poll");
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Init netsnmp agent connection
 *
 * @param[in]  h      Clixon handle
 * @param[in]  logdst Log destination, see clixon_log.h
 * @retval     0      OK
 * @retval    -1      Error
 * @see snmp_terminate
 */
static int
clixon_snmp_init_subagent(clixon_handle h,
                          int           logdst)
{
    int   retval = -1;
    char *sockpath = NULL;

    clixon_debug(CLIXON_DBG_SNMP, "");
    if (logdst == CLIXON_LOG_SYSLOG)
        snmp_enable_calllog();
    else
        snmp_enable_stderrlog();
    /* 0 if master, 1 if client */
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1);
    /* don't load config and don't load/save persistent file */
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PERSIST_STATE, 1);
    /* don't load persistent file */
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD, 1);
    /* don't save persistent file */
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_SAVE, 1);

    if (clixon_debug_get())
        netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_VERBOSE, 1);

    if ((sockpath = clicon_option_str(h, "CLICON_SNMP_AGENT_SOCK")) == NULL){
        clixon_err(OE_XML, 0, "CLICON_SNMP_AGENT_SOCK not set");
        goto done;
    }
    /* XXX: This should be configurable. */
    netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_X_SOCKET, sockpath);

    /* initialize the agent library */
    init_agent(__PROGRAM__);

    /* example-demon will be used to read example-demon.conf files. */
    init_snmp(__PROGRAM__);

    if (!clixon_snmp_api_agent_check()){
        clixon_err(OE_DAEMON, 0, "Connection to SNMP agent failed");
        goto done;
    }
    if (set_signal(SIGTERM, clixon_snmp_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGINT, clixon_snmp_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGPIPE, SIG_IGN, NULL) < 0){
        clixon_err(OE_UNIX, errno, "Setting SIGPIPE signal");
        goto done;
    }
   /* Workaround for netsnmps API use of fdset:s instead of sockets */
    if (clixon_snmp_fdset_register(h, 1) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Usage help routine
 *
 * @param[in]  h      Clixon handle
 * @param[in]  argv0  command line
 */
static void
usage(clixon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
            "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-V \t\tPrint version and exit\n"
            "\t-D <level> \tDebug level (see available levels below)\n"
            "\t-f <file>\tConfiguration file (mandatory)\n"
            "\t-l (e|o|s|f<file>) Log on std(e)rr, std(o)ut, (s)yslog(default), (f)ile\n"
            "\t-C <format>\tDump configuration options on stdout after loading. Format is xml|json|text\n"
            "\t-z\t\tKill other %s daemon and exit\n"
            "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
            argv0, argv0
            );
    fprintf(stderr, "Debug keys: ");
    clixon_debug_key_dump(stderr);
    fprintf(stderr, "\n");
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int            retval = -1;
    int            c;
    char          *argv0 = argv[0];
    clixon_handle  h;
    int            logdst = CLIXON_LOG_STDERR;
    struct passwd *pw;
    yang_stmt     *yspec = NULL;
    char          *str;
    uint32_t       id;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    int            dbg = 0;
    size_t         sz;
    int            pid;
    char          *pidfile = NULL;
    struct stat    st;
    int            zap = 0;
    int           config_dump = 0;
    enum format_enum config_dump_format = FORMAT_XML;
    int              print_version = 0;
    int32_t          d;

    /* Create handle */
    if ((h = clixon_handle_init()) == NULL)
        return -1;
    /* In the startup, logs to stderr & debug flag set later */
    if (clixon_log_init(h, __PROGRAM__, LOG_INFO, logdst) < 0)
        goto done;
    if (clixon_err_init(h) < 0)
        goto done;

    /* Set username to clixon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
        clixon_err(OE_UNIX, errno, "getpwuid");
        goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
        goto done;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
        switch (c) {
        case 'h' : /* help */
            usage(h, argv[0]);
            break;
        case 'V': /* version */
            cligen_output(stdout, "Clixon version: %s\n", CLIXON_VERSION);
            print_version++; /* plugins may also print versions w ca-version callback */
            break;
        case 'D' : { /* debug */
            int32_t d = 0;
            /* Try first symbolic, then numeric match */
            if ((d = clixon_debug_str2key(optarg)) < 0 &&
                sscanf(optarg, "%d", &d) != 1){
                usage(h, argv[0]);
            }
            dbg |= d;
            break;
        }
        case 'f': /* override config file */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
            break;
        case 'l': /* Log destination: s|e|o */
            if ((d = clixon_logdst_str2key(optarg)) < 0){
                if (optarg[0] == 'f'){ /* Check for special -lf<file> syntax */
                    d = CLIXON_LOG_FILE;
                    if (strlen(optarg) > 1 &&
                        clixon_log_file(optarg+1) < 0)
                        goto done;
                }
                else
                    usage(h, argv[0]);
            }
            logdst = d;
            break;
        }
    if (print_version)
        goto ok;
    /*
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clixon_log_init(h, __PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clixon_debug_init(h, dbg);
    /* This is netsnmplib debugging which is quite extensive + only if compiled w debug */
    if (dbg > 1)
        snmp_set_do_debugging(1);
    /*
     * Register error category and error/log callbacks for netsnmp special error handling
     */
    if (clixon_err_cat_reg(OE_SNMP,              /* category */
                           h,                    /* handle (can be NULL) */
                           clixon_snmp_err_cb    /* log fn */
                           ) < 0)
        goto done;
    yang_init(h);
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
        goto done;
    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
        switch (c) {
        case 'h' : /* help */
        case 'D' : /* debug */
        case 'f':  /* config file */
        case 'l':  /* log  */
            break; /* see above */
        case 'C': /* Explicitly dump configuration */
            if ((config_dump_format = format_str2int(optarg)) ==  (enum format_enum)-1){
                fprintf(stderr, "Unrecognized dump format: %s(expected: xml|json|text)\n", argv[0]);
                usage(h, argv[0]);
            }
            config_dump++;
            break;
        case 'o':{ /* Configuration option */
            char          *val;
            if ((val = index(optarg, '=')) == NULL)
                usage(h, argv0);
            *val++ = '\0';
            if (clicon_option_add(h, optarg, val) < 0)
                goto done;
            break;
        }
        case 'z': /* Zap other process */
            zap++;
            break;
        default:
            usage(h, argv[0]);
            break;
        }
    argc -= optind;
    argv += optind;

    /* Read debug and log options from config file if not given by command-line */
    if (clixon_options_main_helper(h, dbg, logdst, __PROGRAM__) < 0)
        goto done;
    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Check pid-file, if zap kill the old daemon, else return here */
    if ((pidfile = clicon_snmp_pidfile(h)) == NULL){
        clixon_err(OE_FATAL, 0, "pidfile not set");
        goto done;
    }
    if (pidfile_get(pidfile, &pid) < 0)
        goto done;
    if (zap){
        if (pid && pidfile_zapold(pid) < 0)
            return -1;
        if (lstat(pidfile, &st) == 0)
            unlink(pidfile);
        snmp_terminate(h);
        exit(0); /* OK */
    }
    else if (pid){
        clixon_err(OE_DAEMON, 0, "Clixon_snmp daemon already running with pid %d\n(Try killing it with %s -z)",
                   pid, argv0);
        return -1; /* goto done deletes pidfile */
    }
    /* Here there is either no old process or we have killed it,.. */
    if (lstat(pidfile, &st) == 0)
        unlink(pidfile);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    if ((sz = clicon_option_int(h, "CLIXON_LOG_STRING_LIMIT")) != 0)
        clixon_log_string_limit_set(sz);

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);
    yang_start(h);
    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
        goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new1(h, YANG_DOMAIN_TOP, YANG_DATA_TOP)) == NULL)
        goto done;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
        if (yang_spec_parse_file(h, str, yspec) < 0)
            goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
        if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
                                   yspec) < 0)
            goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
        if (yang_spec_load_dir(h, str, yspec) < 0)
            goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
        goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
        goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
        goto done;
    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
        goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
        goto done;

    if (config_dump){
        if (clicon_option_dump1(h, stdout, config_dump_format, 1) < 0)
            goto done;
        goto ok;
    }
    clicon_option_dump(h, CLIXON_DBG_INIT);

    /* Send hello request to backend to get session-id back
     * This is done once at the beginning of the session and then this is
     * used by the client, even though new TCP sessions are created for
     * each message sent to the backend.
     */
    clicon_data_set(h, "session-transport", "cl:snmp");
    if (clicon_hello_req(h, "cl:snmp", NULL, &id) < 0)
        goto done;
    clicon_session_id_set(h, id);
    /* Init snmp as subagent */
    if (clixon_snmp_init_subagent(h, logdst) < 0)
        goto done;

    /* Init and traverse mib-translated yangs and register callbacks */
    if (clixon_snmp_traverse_mibyangs(h) < 0)
        goto done;
    /* init snmp stream (traps) */
    if (clixon_snmp_stream_init(h) < 0)
        goto done;

    /* Write pid-file */
    if (pidfile_write(pidfile) <  0)
        goto done;
    /* main event loop */
    if (clixon_event_loop(h) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    clixon_log_init(h, __PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clixon_log(h, LOG_NOTICE, "%s: %u Terminated", __PROGRAM__, getpid());
    snmp_terminate(h);
    return retval;
}
