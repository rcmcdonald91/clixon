/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"

#include "cli_common.h"


/*! Register log notification stream
 * @param[in] h       Clicon handle
 * @param[in] stream  Event stream. CLICON is predefined, others are application-defined
 * @param[in] filter  Filter. For xml notification ie xpath: .[name="kalle"]
 * @param[in] status  0 for stop, 1 to start
 * @param[in] fn      Callback function called when notification occurs
 * @param[in] arg     Argument to function note
 * Note this calls cligen_regfd which may callback on cli command interpretator
 */
int
cli_notification_register(clicon_handle    h, 
			  char            *stream, 
			  enum format_enum format,
			  char            *filter, 
			  int              status, 
			  int            (*fn)(int, void*),
			  void            *arg)
{
    int              retval = -1;
    char            *logname = NULL;
    void            *p;
    int              s;
    clicon_hash_t   *cdat = clicon_data(h);
    size_t           len;
    int              s_exist = -1;

    len = strlen("log_socket_") + strlen(stream) + 1;
    if ((logname = malloc(len)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }	
    snprintf(logname, len, "log_socket_%s", stream);
    if ((p = clicon_hash_value(cdat, logname, &len)) != NULL)
	s_exist = *(int*)p;

    if (status){ /* start */
	if (s_exist!=-1){
	    clicon_err(OE_PLUGIN, 0, "Result log socket already exists");
	    goto done;
	}
	if (clicon_rpc_create_subscription(h, stream, filter, &s) < 0)
	    goto done;
	if (cligen_regfd(s, fn, arg) < 0)
	    goto done;
	if (clicon_hash_add(cdat, logname, &s, sizeof(s)) == NULL)
	    goto done;
    }
    else{ /* stop */
	if (s_exist != -1){
	    cligen_unregfd(s_exist);
	}
	clicon_hash_del(cdat, logname);
#if 0 /* cant turn off */
	if (clicon_rpc_create_subscription(h, status, stream, format, filter, NULL) < 0)
	    goto done;
#endif
    }
    retval = 0;
  done:
    if (logname)
	free(logname);
    return retval;
}

/* Signal functions, not exported to API */
void
cli_signal_block(clicon_handle h)
{
	clicon_signal_block (SIGTSTP);
	clicon_signal_block (SIGQUIT);
	clicon_signal_block (SIGCHLD);
	if (!clicon_quiet_mode(h))
	    clicon_signal_block (SIGINT);
}

void
cli_signal_unblock(clicon_handle h)
{
	clicon_signal_unblock (SIGTSTP);
	clicon_signal_unblock (SIGQUIT);
	clicon_signal_unblock (SIGCHLD);
	clicon_signal_unblock (SIGINT);
}

/*
 * Flush pending signals for a given signal type
 */
void
cli_signal_flush(clicon_handle h)
{
    /* XXX A bit rough. Use sigpending() and more clever logic ?? */

    sigfn_t   h1, h2, h3, h4;

    set_signal (SIGTSTP, SIG_IGN, &h1);
    set_signal (SIGQUIT, SIG_IGN, &h2);
    set_signal (SIGCHLD, SIG_IGN, &h3);
    set_signal (SIGINT, SIG_IGN, &h4);

    cli_signal_unblock (h);

    set_signal (SIGTSTP, h1, NULL);
    set_signal (SIGQUIT, h2, NULL);
    set_signal (SIGCHLD, h3, NULL);
    set_signal (SIGINT, h4, NULL);

    cli_signal_block (h);
}

/*! Transform data 
 * Add next-last cvv (resolved variable) as body to xml bottom for leaf and 
 * leaf-list.
 * There may be some translation necessary.
 */
static int
dbxml_body(cxobj     *xbot,
	   yang_stmt *ybot,
	   cvec      *cvv)
{
    int     retval = -1;
    char   *str = NULL;
    cxobj  *xb;
    cg_var *cval;
    int     len;

    len = cvec_len(cvv);
    cval = cvec_i(cvv, len-1); 
    if ((str = cv2str_dup(cval)) == NULL){
	clicon_err(OE_UNIX, errno, "cv2str_dup");
	goto done;
    }
    if ((xb = xml_new("body", xbot, NULL)) == NULL)
	goto done; 
    xml_type_set(xb, CX_BODY);
    if (xml_value_set(xb,  str) < 0)
	goto done;
    retval = 0;
 done:
    if (str)
	free(str);
    return retval;
}

/*! Modify xml datastore from a callback using xml key format strings
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @param[in]  op   Operation to perform on database
 * Cvv will contain first the complete cli string, and then a set of optional
 * instantiated variables.
 * Example:
 * cvv[0]  = "set interfaces interface eth0 type bgp"
 * cvv[1]  = "eth0"
 * cvv[2]  = "bgp"
 * argv[0] = "/interfaces/interface/%s/type"
 * op: OP_MERGE
 * @see cli_callback_generate where arg is generated
 */
static int
cli_dbxml(clicon_handle       h, 
	  cvec               *cvv, 
	  cvec               *argv, 
	  enum operation_type op)
{
    int        retval = -1;
    char      *api_path_fmt;    /* xml key format */
    char      *api_path = NULL; /* xml key */
    cg_var    *arg;
    cbuf      *cb = NULL;
    yang_stmt *yspec;
    cxobj     *xbot = NULL;     /* xpath, NULL if datastore */
    yang_stmt *y = NULL;        /* yang spec of xpath */
    cxobj     *xtop = NULL;     /* xpath root */
    cxobj     *xa;              /* attribute */
    cxobj     *xerr = NULL;
    int        ret;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires one element to be xml key format string");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    arg = cvec_i(argv, 0);
    api_path_fmt = cv_string_get(arg);
    if (api_path_fmt2api_path(api_path_fmt, cvv, &api_path) < 0)
	goto done;
    /* Create config top-of-tree */
    if ((xtop = xml_new("config", NULL, NULL)) == NULL)
	goto done;
    xbot = xtop;
    if (api_path){
	if ((ret = api_path2xml(api_path, yspec, xtop, YC_DATANODE, 1, &xbot, &y, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    clicon_rpc_generate_error("Modify datastore", xerr);
	    goto done;
	}
    }
    if ((xa = xml_new("operation", xbot, NULL)) == NULL)
	goto done;
    xml_type_set(xa, CX_ATTR);
    xml_prefix_set(xa, NETCONF_BASE_PREFIX); 
    if (xml_value_set(xa, xml_operation2str(op)) < 0)
	goto done;
    if (yang_keyword_get(y) != Y_LIST && yang_keyword_get(y) != Y_LEAF_LIST){
	if (cvec_len(cvv) > 1 &&
	    dbxml_body(xbot, y, cvv) < 0)
	    goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_xml2cbuf(cb, xtop, 0, 0, -1) < 0)
	goto done;
    if (clicon_rpc_edit_config(h, "candidate", OP_NONE, cbuf_get(cb)) < 0)
	goto done;
    if (clicon_autocommit(h)) {
	if (clicon_rpc_commit(h) < 0) 
	    goto done;
    }
    retval = 0;
 done:
    if (xerr)
	xml_free(xerr);
    if (cb)
	cbuf_free(cb);
    if (api_path)
	free(api_path);  
    if (xtop)
	xml_free(xtop);
    return retval;
}

/*! Set datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_set(clicon_handle h,
	cvec         *cvv,
	cvec         *argv)
{
    int retval = -1;

    if (cli_dbxml(h, cvv, argv, OP_REPLACE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Merge datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_merge(clicon_handle h,
	  cvec         *cvv,
	  cvec         *argv)
{
    int retval = -1;

    if (cli_dbxml(h, cvv, argv, OP_MERGE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Create datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_create(clicon_handle h,
	   cvec         *cvv,
	   cvec         *argv)
{
    int retval = -1;

    if (cli_dbxml(h, cvv, argv, OP_CREATE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}
/*! Remove datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_del
 */
int 
cli_remove(clicon_handle h,
	   cvec         *cvv,
	   cvec         *argv)
{
    int retval = -1;

    if (cli_dbxml(h, cvv, argv, OP_REMOVE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Delete datastore xml
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_del(clicon_handle h,
	cvec         *cvv,
	cvec         *argv)
{
    int   retval = -1;

    if (cli_dbxml(h, cvv, argv, OP_REMOVE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Set debug level on CLI client (not backend daemon)
 * @param[in] h     Clicon handle
 * @param[in] vars  If variable "level" exists, its integer value is used
 * @param[in] arg   Else use the integer value of argument
 * @note The level is either what is specified in arg as int argument.
 *       _or_ if a 'level' variable is present in vars use that value instead.
 */
int
cli_debug_cli(clicon_handle h, 
	       cvec         *vars, 
	       cvec         *argv)
{
    int     retval = -1;
    cg_var *cv;
    int     level;

    if ((cv = cvec_find(vars, "level")) == NULL){
	if (cvec_len(argv) != 1){
	    clicon_err(OE_PLUGIN, 0, "Requires either label var or single arg: 0|1");
	    goto done;
	}
	cv = cvec_i(argv, 0);
    }
    level = cv_int32_get(cv);
    /* cli */
    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */
    retval = 0;
 done:
    return retval;
}

/*! Set debug level on backend daemon (not CLI)
 * @param[in] h     Clicon handle
 * @param[in] vars  If variable "level" exists, its integer value is used
 * @param[in] arg   Else use the integer value of argument
 * @note The level is either what is specified in arg as int argument.
 *       _or_ if a 'level' variable is present in vars use that value instead.
 */
int
cli_debug_backend(clicon_handle h, 
		  cvec         *vars, 
		  cvec         *argv)
{
    int     retval = -1;
    cg_var *cv;
    int     level;

    if ((cv = cvec_find(vars, "level")) == NULL){
	if (cvec_len(argv) != 1){
	    clicon_err(OE_PLUGIN, 0, "Requires either label var or single arg: 0|1");
	    goto done;
	}
	cv = cvec_i(argv, 0);
    }
    level = cv_int32_get(cv);
    /* config daemon */
    retval = clicon_rpc_debug(h, level);
 done:
    return retval;
}

/*! Set debug level on restconf daemon
 * @param[in] h     Clicon handle
 * @param[in] vars  If variable "level" exists, its integer value is used
 * @param[in] arg   Else use the integer value of argument
 * @note The level is either what is specified in arg as int argument.
 *       _or_ if a 'level' variable is present in vars use that value instead.
 */
int
cli_debug_restconf(clicon_handle h, 
		   cvec         *vars, 
		   cvec         *argv)
{
    int     retval = -1;
    cg_var *cv;
    int     level;

    if ((cv = cvec_find(vars, "level")) == NULL){
	if (cvec_len(argv) != 1){
	    clicon_err(OE_PLUGIN, 0, "Requires either label var or single arg: 0|1");
	    goto done;
	}
	cv = cvec_i(argv, 0);
    }
    level = cv_int32_get(cv);
    /* restconf daemon */
    if (0) /* XXX notyet */
	retval = clicon_rpc_debug(h, level);
 done:
    return retval;
}


/*! Set syntax mode
 */
int
cli_set_mode(clicon_handle h, 
	      cvec         *vars, 
	      cvec         *argv)
{
    int     retval = -1;
    char   *str = NULL;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires one element to be cli mode");
	goto done;
    }
    str = cv_string_get(cvec_i(argv, 0));
    cli_set_syntax_mode(h, str);
    retval = 0;
  done:
    return retval;
}

/*! Start bash from cli callback
 * XXX Application specific??
 * XXX replace fprintf with clicon_err?
 */ 
int
cli_start_shell(clicon_handle h, 
		cvec         *vars, 
		cvec         *argv)
{
    char          *cmd;
    struct passwd *pw;
    int            retval = -1;
    char           bcmd[128];
    cg_var        *cv1 = cvec_i(vars, 1);

    cmd = (cvec_len(vars)>1 ? cv_string_get(cv1) : NULL);

    if ((pw = getpwuid(getuid())) == NULL){
	fprintf(stderr, "%s: getpwuid: %s\n", 
               __FUNCTION__, strerror(errno));
	goto done;
    }
    if (chdir(pw->pw_dir) < 0){
	fprintf(stderr, "%s: chdir(%s): %s\n",
		__FUNCTION__, pw->pw_dir, strerror(errno));
	endpwent();
	goto done;
    }
    endpwent();
    cli_signal_flush(h);
    cli_signal_unblock(h);
    if (cmd){
	snprintf(bcmd, 128, "bash -l -c \"%s\"", cmd);
	if (system(bcmd) < 0){
	    cli_signal_block(h);
	    fprintf(stderr, "%s: system(bash -c): %s\n", 
		    __FUNCTION__, strerror(errno));
	    goto done;
	}
    }
    else
	if (system("bash -l") < 0){
	    cli_signal_block(h);
	    fprintf(stderr, "%s: system(bash): %s\n", 
		    __FUNCTION__, strerror(errno));
	    goto done;
	}
    cli_signal_block(h);
#if 0 /* Allow errcodes from bash */
    if (retval != 0){
	fprintf(stderr, "%s: system(%s) code=%d\n", __FUNCTION__, cmd, retval);
	goto done;
    }
#endif
    retval = 0;
 done:
    return retval;
}

/*! Generic quit callback
 */
int 
cli_quit(clicon_handle h, 
	  cvec         *vars, 
	  cvec         *argv)
{
    cligen_exiting_set(cli_cligen(h), 1);
    return 0;
}

/*! Generic commit callback
 * @param[in]  argv No arguments expected
 */
int
cli_commit(clicon_handle h, 
	    cvec         *vars, 
	    cvec         *argv)
{
    int            retval = -1;
    
    if ((retval = clicon_rpc_commit(h)) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Generic validate callback
 */
int
cli_validate(clicon_handle h, 
	      cvec         *vars, 
	      cvec         *argv)
{
    int     retval = -1;

    if ((retval = clicon_rpc_validate(h, "candidate")) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Compare two dbs using XML. Write to file and run diff
 */
static int
compare_xmls(cxobj *xc1, 
	     cxobj *xc2, 
	     int    astext)
{
    int    fd;
    FILE  *f;
    char   filename1[MAXPATHLEN];
    char   filename2[MAXPATHLEN];
    char   cmd[MAXPATHLEN];
    int    retval = -1;
    cxobj *xc;

    snprintf(filename1, sizeof(filename1), "/tmp/cliconXXXXXX");
    snprintf(filename2, sizeof(filename2), "/tmp/cliconXXXXXX");
    if ((fd = mkstemp(filename1)) < 0){
	clicon_err(OE_UNDEF, errno, "tmpfile: %s", strerror (errno));
	goto done;
    }
    if ((f = fdopen(fd, "w")) == NULL)
	goto done;
    xc = NULL;
    if (astext)
	while ((xc = xml_child_each(xc1, xc, -1)) != NULL)
	    xml2txt(f, xc, 0);
    else
	while ((xc = xml_child_each(xc1, xc, -1)) != NULL)
	    xml_print(f, xc);

    fclose(f);
    close(fd);

    if ((fd = mkstemp(filename2)) < 0){
	clicon_err(OE_UNDEF, errno, "mkstemp: %s", strerror (errno));
	goto done;
    }
    if ((f = fdopen(fd, "w")) == NULL)
	goto done;
    xc = NULL;
    if (astext)
	while ((xc = xml_child_each(xc2, xc, -1)) != NULL)
	    xml2txt(f, xc, 0);
    else
	while ((xc = xml_child_each(xc2, xc, -1)) != NULL)
	    xml_print(f, xc);
    fclose(f);
    close(fd);

    snprintf(cmd, sizeof(cmd), "/usr/bin/diff -dU 1 %s %s |  grep -v @@ | sed 1,2d", 		 filename1, filename2);
    if (system(cmd) < 0)
	goto done;

    retval = 0;
  done:
    unlink(filename1);
    unlink(filename2);
    return retval;
}

/*! Compare two dbs using XML. Write to file and run diff
 * @param[in]   h     Clicon handle
 * @param[in]   cvv  
 * @param[in]   arg   arg: 0 as xml, 1: as text
 */
int
compare_dbs(clicon_handle h, 
	    cvec         *cvv, 
	    cvec         *argv)
{
    cxobj *xc1 = NULL; /* running xml */
    cxobj *xc2 = NULL; /* candidate xml */
    cxobj *xerr = NULL;
    int    retval = -1;
    int    astext;

    if (cvec_len(argv) > 1){
	clicon_err(OE_PLUGIN, 0, "Requires 0 or 1 element. If given: astext flag 0|1");
	goto done;
    }
    if (cvec_len(argv))
	astext = cv_int32_get(cvec_i(argv, 0));
    else
	astext = 0;
    if (clicon_rpc_get_config(h, NULL, "running", "/", NULL, &xc1) < 0)
	goto done;
    if ((xerr = xpath_first(xc1, "/rpc-error")) != NULL){
	clicon_rpc_generate_error("Get configuration", xerr);
	goto done;
    }
    if (clicon_rpc_get_config(h, NULL, "candidate", "/", NULL, &xc2) < 0)
	goto done;
    if ((xerr = xpath_first(xc2, "/rpc-error")) != NULL){
	clicon_rpc_generate_error("Get configuration", xerr);
	goto done;
    }
    if (compare_xmls(xc1, xc2, astext) < 0) /* astext? */
	goto done;
    retval = 0;
  done:
    if (xc1)
	xml_free(xc1);    
    if (xc2)
	xml_free(xc2);
    return retval;
}

/*! Load a configuration file to candidate database
 * Utility function used by cligen spec file
 * @param[in] h     CLICON handle
 * @param[in] cvv   Vector of variables (where <varname> is found)
 * @param[in] argv  A string: "<varname> (merge|replace)" 
 *   <varname> is name of a variable occuring in "cvv" containing filename
 * @note that "filename" is local on client filesystem not backend. 
 * @note file is assumed to have a dummy top-tag, eg <clicon></clicon>
 * @code
 *   # cligen spec
 *   load file <name2:string>, load_config_file("name2","merge");
 * @endcode
 * @see save_config_file
 */
int 
load_config_file(clicon_handle h, 
		 cvec         *cvv, 
		 cvec         *argv)
{
    int         ret = -1;
    struct stat st;
    char       *filename = NULL;
    int         replace;
    cg_var     *cv;
    char       *opstr;
    char       *varstr;
    int         fd = -1;
    cxobj      *xt = NULL;
    cxobj      *x;
    cbuf       *cbxml;

    if (cvec_len(argv) != 2){
	if (cvec_len(argv)==1)
	    clicon_err(OE_PLUGIN, 0, "Got single argument:\"%s\". Expected \"<varname>,<op>\"", cv_string_get(cvec_i(argv,0)));
	else
	    clicon_err(OE_PLUGIN, 0, "Got %d arguments. Expected: <varname>,<op>", cvec_len(argv));
	goto done;
    }
    varstr = cv_string_get(cvec_i(argv, 0));
    opstr  = cv_string_get(cvec_i(argv, 1));
    if (strcmp(opstr, "merge") == 0) 
	replace = 0;
    else if (strcmp(opstr, "replace") == 0) 
	replace = 1;
    else{
	clicon_err(OE_PLUGIN, 0, "No such op: %s, expected merge or replace", opstr);	
	goto done;
    }
    if ((cv = cvec_find(cvv, varstr)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such var name: %s", varstr);	
	goto done;
    }
    filename = cv_string_get(cv);
    if (stat(filename, &st) < 0){
 	clicon_err(OE_UNIX, 0, "load_config: stat(%s): %s", 
		   filename, strerror(errno));
	goto done;
    }
    /* Open and parse local file into xml */
    if ((fd = open(filename, O_RDONLY)) < 0){
	clicon_err(OE_UNIX, errno, "open(%s)", filename);
	goto done;
    }
    if (xml_parse_file(fd, "</clicon>", NULL, &xt) < 0)
	goto done;
    if (xt == NULL)
	goto done;
    if ((cbxml = cbuf_new()) == NULL)
	goto done;
    x = NULL;
    while ((x = xml_child_each(xt, x, -1)) != NULL) {
	/* Ensure top-level is "config", maybe this is too rough? */
	xml_name_set(x, "config");
	if (clicon_xml2cbuf(cbxml, x, 0, 0, -1) < 0)
	    goto done;
    }
    if (clicon_rpc_edit_config(h, "candidate",
			       replace?OP_REPLACE:OP_MERGE, 
			       cbuf_get(cbxml)) < 0)
	goto done;
    cbuf_free(cbxml);
    //    }
    ret = 0;
 done:
    if (xt)
	xml_free(xt);
    if (fd != -1)
	close(fd);
    return ret;
}

/*! Copy database to local file 
 * Utility function used by cligen spec file
 * @param[in] h     CLICON handle
 * @param[in] cvv  variable vector (containing <varname>)
 * @param[in] argv  a string: "<dbname> <varname>" 
 *   <dbname>  is running, candidate, or startup
 *   <varname> is name of cligen variable in the "cvv" vector containing file name
 * Note that "filename" is local on client filesystem not backend.
 * The function can run without a local database
 * @note The file is saved with dummy top-tag: clicon: <clicon></clicon>
 * @code
 *   save file <name:string>, save_config_file("running name");
 * @endcode
 * @see load_config_file
 */
int
save_config_file(clicon_handle h, 
		 cvec         *cvv, 
		 cvec         *argv)
{
    int        retval = -1;
    char      *filename = NULL;
    cg_var    *cv;
    char      *dbstr;
    char      *varstr;
    cxobj     *xt = NULL;
    cxobj     *xerr;
    FILE      *f = NULL;

    if (cvec_len(argv) != 2){
	if (cvec_len(argv)==1)
	    clicon_err(OE_PLUGIN, 0, "Got single argument:\"%s\". Expected \"<dbname>,<varname>\"",
		       cv_string_get(cvec_i(argv,0)));
	else
	    clicon_err(OE_PLUGIN, 0, " Got %d arguments. Expected: <dbname>,<varname>",
		       cvec_len(argv));

	goto done;
    }
    dbstr = cv_string_get(cvec_i(argv, 0));
    varstr  = cv_string_get(cvec_i(argv, 1));
    if (strcmp(dbstr, "running") != 0 && 
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0) {
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_find(cvv, varstr)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such var name: %s", varstr);	
	goto done;
    }
    filename = cv_string_get(cv);
    if (clicon_rpc_get_config(h, NULL, dbstr,"/", NULL, &xt) < 0)
	goto done;
    if (xt == NULL){
	clicon_err(OE_CFG, 0, "get config: empty tree"); /* Shouldnt happen */
	goto done;
    }
    if ((xerr = xpath_first(xt, "/rpc-error")) != NULL){
	clicon_rpc_generate_error("Get configuration", xerr);
	goto done;
    }
    /* get-config returns a <data> tree. Save as <config> tree so it can be used
     * as data-store.
     */
    if (xml_name_set(xt, "config") < 0)
	goto done;
    if ((f = fopen(filename, "w")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", filename);
	goto done;
    } 
    if (clicon_xml2file(f, xt, 0, 1) < 0)
	goto done;
    retval = 0;
    /* Fall through */
  done:
    if (xt)
	xml_free(xt);
    if (f != NULL)
	fclose(f);
    return retval;
}

/*! Delete all elements in a database 
 * Utility function used by cligen spec file
 */
int
delete_all(clicon_handle h, 
	   cvec         *cvv, 
	   cvec         *argv)
{
    char            *dbstr;
    int              retval = -1;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires one element: dbname");
	goto done;
    }
    dbstr = cv_string_get(cvec_i(argv, 0));
    if (strcmp(dbstr, "running") != 0 && 
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if (clicon_rpc_delete_config(h, dbstr) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Discard all changes in candidate and replace with running
 */
int
discard_changes(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    return clicon_rpc_discard_changes(h);

}
/*! Copy from one database to another, eg running->startup
 * @param[in] argv  a string: "<db1> <db2>" Copy from db1 to db2
 */
int
db_copy(clicon_handle h, 
	cvec         *cvv, 
	cvec         *argv)
{
    char *db1;
    char *db2;

    db1 = cv_string_get(cvec_i(argv, 0));
    db2 = cv_string_get(cvec_i(argv, 1));
    return clicon_rpc_copy_config(h, db1, db2);
}

/*! This is the callback used by cli_setlog to print log message in CLI
 * param[in]  s    UNIX socket from backend  where message should be read
 * param[in]  arg  format: txt, xml, xml2txt, xml2json
 */
static int
cli_notification_cb(int   s, 
		    void *arg)
{
    struct clicon_msg *reply = NULL;
    int                eof;
    int                retval = -1;
    cxobj             *xt = NULL;
    cxobj             *xe;
    cxobj             *x;
    enum format_enum format = (enum format_enum)arg;
    
    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof) < 0)
	goto done;
    if (eof){
	clicon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
	close(s);
	errno = ESHUTDOWN;
	event_unreg_fd(s, cli_notification_cb);
	goto done;
    }
    if (clicon_msg_decode(reply, NULL, NULL, &xt) < 0) /* XXX pass yang_spec */
	goto done;
    if ((xe = xpath_first(xt, "//event")) != NULL){
	x = NULL;
	while ((x = xml_child_each(xe, x, -1)) != NULL) {
	    switch (format){
	    case FORMAT_XML:
		if (clicon_xml2file(stdout, x, 0, 1) < 0)
		    goto done;
		break;
	    case FORMAT_TEXT:
		if (xml2txt(stdout, x, 0) < 0)
		    goto done;
		break;
	    case FORMAT_JSON:
		if (xml2json(stdout, x, 1) < 0)
		    goto done;
		break;
	    default:
		break;
	    }
	}
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    if (reply)
	free(reply);
    return retval;
}

/*! Make a notify subscription to backend and un/register callback for return messages.
 * 
 * @param[in] h      Clicon handle
 * @param[in] cvv    Not used
 * @param[in] arg    A string with <log stream name> <stream status> [<format>]
 * where <status> is "0" or "1"
 * and   <format> is XXX
 * Example code: Start logging of mystream and show logs as xml
 * @code
 * cmd("comment"), cli_notify("mystream","1","xml"); 
 * @endcode
 * XXX: format is a memory leak
 */
int
cli_notify(clicon_handle h, 
	   cvec         *cvv, 
	   cvec         *argv)
{
    char            *stream = NULL;
    int              retval = -1;
    int              status;
    char            *formatstr = NULL;
    enum format_enum format = FORMAT_TEXT;

    if (cvec_len(argv) != 2 && cvec_len(argv) != 3){
	clicon_err(OE_PLUGIN, 0, "Requires arguments: <logstream> <status> [<format>]");
	goto done;
    }
    stream = cv_string_get(cvec_i(argv, 0));
    status  = atoi(cv_string_get(cvec_i(argv, 1)));
    if (cvec_len(argv) > 2){
	formatstr = cv_string_get(cvec_i(argv, 2));
	format = format_str2int(formatstr);
    }
    if (cli_notification_register(h, 
				  stream, 
				  format,
				  "", 
				  status, 
				  cli_notification_cb, 
				  (void*)format) < 0)
	goto done;

    retval = 0;
  done:
    return retval;
}

/*! Lock database
 * 
 * @param[in] h      Clicon handle
 * @param[in] cvv    Not used
 * @param[in] arg    A string with <database> 
 * @code
 * lock("comment"), cli_lock("running"); 
 * @endcode
 * XXX: format is a memory leak
 */
int
cli_lock(clicon_handle h, 
	 cvec         *cvv, 
	 cvec         *argv)
{
    char            *db;
    int              retval = -1;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires arguments: <db>");
	goto done;
    }
    db = cv_string_get(cvec_i(argv, 0));
    if (clicon_rpc_lock(h, db) < 0) 
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Unlock database
 * 
 * @param[in] h      Clicon handle
 * @param[in] cvv    Not used
 * @param[in] arg    A string with <database> 
 * @code
 * lock("comment"), cli_lock("running"); 
 * @endcode
 * XXX: format is a memory leak
 */
int
cli_unlock(clicon_handle h, 
	   cvec         *cvv, 
	   cvec         *argv)
{
    char            *db;
    int              retval = -1;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "Requires arguments: <db>");
	goto done;
    }
    db = cv_string_get(cvec_i(argv, 0));
    if (clicon_rpc_unlock(h, db) < 0) 
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Copy one configuration object to antother
 *
 * Works for objects that are items ina yang list with a keyname, eg as:
 *   list sender{ 
 *      key name;	
 *	leaf name{...
 *
 * @param[in]  h    CLICON handle
 * @param[in]  cvv  Vector of variables from CLIgen command-line
 * @param[in]  argv Vector: <db>, <xpath>, <field>, <fromvar>, <tovar>
 * Explanation of argv fields:
 *  db:        Database name, eg candidate|tmp|startup
 *  xpath:     XPATH expression with exactly two %s pointing to field and from name
 *  namespace: XPATH default namespace
 *  field:     Name of list key, eg name
 *  fromvar:   Name of variable containing name of object to copy from (given by xpath)
 *  tovar:     Name of variable containing name of object to copy to.
 * @code
 * cli spec:
 *  copy snd <n1:string> to <n2:string>, cli_copy_config("candidate", "/sender[%s='%s']", "urn:example:clixon", "from", "n1", "n2");
 * cli command:
 *  copy snd from to to
 * @endcode
 */
int
cli_copy_config(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    int          retval = -1;
    char        *db;
    cxobj       *x1 = NULL; 
    cxobj       *x2 = NULL; 
    cxobj       *x;
    char        *xpath;
    char        *namespace;
    int          i;
    int          j;
    cbuf        *cb = NULL;
    char        *keyname;
    char        *fromvar;
    cg_var      *fromcv;
    char        *fromname = NULL;
    char        *tovar;
    cg_var      *tocv;
    char        *toname;
    cxobj       *xerr;
    cvec        *nsc = NULL;

    if (cvec_len(argv) != 6){
	clicon_err(OE_PLUGIN, 0, "Requires 6 elements: <db> <xpath> <namespace> <keyname> <from> <to>");
	goto done;
    }
    /* First argv argument: Database */
    db = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: xpath */
    xpath = cv_string_get(cvec_i(argv, 1));
    /* Third argv argument: namespace */
    namespace = cv_string_get(cvec_i(argv, 2));
    /* Third argv argument: name of keyname */
    keyname = cv_string_get(cvec_i(argv, 3));
    /* Fourth argv argument: from variable */
    fromvar = cv_string_get(cvec_i(argv, 4));
    /* Fifth argv argument: to variable */
    tovar = cv_string_get(cvec_i(argv, 5));
    
    /* Get from variable -> cv -> from name */
    if ((fromcv = cvec_find(cvv, fromvar)) == NULL){
	clicon_err(OE_PLUGIN, 0, "fromvar '%s' not found in cligen var list", fromvar);	
	goto done;
    }
    /* Get from name from cv */
    fromname = cv_string_get(fromcv);
    /* Create xpath */
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    /* Sanity check that xpath contains exactly two %s, ie [%s='%s'] */
    j = 0;
    for (i=0; i<strlen(xpath); i++){
	if (xpath[i] == '%')
	    j++;
    }
    if (j != 2){
	clicon_err(OE_PLUGIN, 0, "xpath '%s' does not have two '%%'", xpath);	
	goto done;
    }
    cprintf(cb, xpath, keyname, fromname);	
    if ((nsc = xml_nsctx_init(NULL, namespace)) == NULL)
	goto done;
    /* Get from object configuration and store in x1 */
    if (clicon_rpc_get_config(h, NULL, db, cbuf_get(cb), nsc, &x1) < 0)
	goto done;
    if ((xerr = xpath_first(x1, "/rpc-error")) != NULL){
	clicon_rpc_generate_error("Get configuration", xerr);
	goto done;
    }

    /* Get to variable -> cv -> to name */
    if ((tocv = cvec_find(cvv, tovar)) == NULL){
	clicon_err(OE_PLUGIN, 0, "tovar '%s' not found in cligen var list", tovar);
	goto done;
    }
    toname = cv_string_get(tocv);
    /* Create copy xml tree x2 */
    if ((x2 = xml_new("new", NULL, NULL)) == NULL)
	goto done;
    if (xml_copy(x1, x2) < 0)
	goto done;
    xml_name_set(x2, "config");
    cprintf(cb, "/%s", keyname);	

    if ((x = xpath_first_nsc(x2, nsc, "%s", cbuf_get(cb))) == NULL){
	clicon_err(OE_PLUGIN, 0, "Field %s not found in copy tree", keyname);
	goto done;
    }
    x = xml_find(x, "body");
    xml_value_set(x, toname);
    /* resuse cb */
    cbuf_reset(cb);
    /* create xml copy tree and merge it with database configuration */
    clicon_xml2cbuf(cb, x2, 0, 0, -1);
    if (clicon_rpc_edit_config(h, db, OP_MERGE, cbuf_get(cb)) < 0)
	goto done;
    retval = 0;
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (cb)
	cbuf_free(cb);
    if (x1 != NULL)
	xml_free(x1);
    if (x2 != NULL)
	xml_free(x2);
    return retval;
}


/*! set debug level on stderr (not syslog).
 * The level is either what is specified in arg as int argument.
 * _or_ if a 'level' variable is present in vars use that value instead.
 * XXX obsolete. Use cli_debug_cliv or cli_debug_backendv instead
 */
int
cli_debug(clicon_handle h, 
	  cvec         *vars, 
	  cg_var       *arg)
{
    cg_var *cv;
    int     level;

    if ((cv = cvec_find(vars, "level")) == NULL)
	cv = arg;
    level = cv_int32_get(cv);
    /* cli */
    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */
    /* config daemon */
    if (clicon_rpc_debug(h, level) < 0)
	goto done;
  done:
    return 0;
}

int
cli_help(clicon_handle h, cvec *vars, cvec *argv)
{
    cligen_handle ch = cli_cligen(h);
    parse_tree   *pt;

    pt = cligen_tree_active_get(ch);
    return cligen_help(stdout, *pt);
}
