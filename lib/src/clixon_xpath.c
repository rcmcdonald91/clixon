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
 *
 * Note on xcur parameter to most xpath functions:
 * The W3 standard defines the document root / element as the top-level.
 * In the clixon xpath API, the document root is defined as the top of the xml tree. 
 * The xcur argument of xpath_first and others is the "current" xml node (xcur) which can
 * be any node in that tree, not necessarily the document root.
 * This is convenient if you want to use relative xpaths from any location in the tree (eg ../../foo/bar).
 * It may be confusing if you expect xcur to be the root node.
 *
 * Some notes on namespace extensions in Netconf/Yang
 * 1) The xpath is not "namespace-aware" in the sense that if you look for a path, eg 
 *    "n:a/n:b", those must match the XML, so they need to match prefixes AND name in the xml 
 *    such as <n:a><n:b>. An xml with <m:a><m:b> (or <a><b>) will NOT match EVEN IF they have the
 *    same namespace given by xmlns settings.
 * 2) RFC6241 8.9.1
 * In the scope of get-.config, the set of namespace declarations are those in scope on the 
 * <filter> element.
 * <rpc message-id="101" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
 *    <get-config>
 *       <filter xmlns:t="http://example.com/schema/1.2/config"
 *               type="xpath"
 *               select="/t:top/t:users/t:user[t:name='fred']"/>
 *       </get-config>
 * One observation is that the namespace context is static, so it can not be a part
 * of the xpath-tree, which is context-dependent. 
 * Best is to send it as a (read-only) parameter to the xp_eval family of functions
 * as an exlicit namespace context.
 * 
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
#include <math.h>  /* NaN */

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
#include "clixon_xpath_parse.h"
#include "clixon_xpath_eval.h"

/*
 * Variables
 */

/* Mapping between xpath_tree node name string <--> int  
 * @see xpath_tree_int2str
 */
static const map_str2int xpath_tree_map[] = {
    {"expr",             XP_EXP},
    {"andexpr",          XP_AND},
    {"relexpr",          XP_RELEX},
    {"addexpr",          XP_ADD},
    {"unionexpr",        XP_UNION},
    {"pathexpr",         XP_PATHEXPR},
    {"filterexpr",       XP_FILTEREXPR},
    {"locationpath",     XP_LOCPATH},
    {"abslocpath",       XP_ABSPATH},
    {"rellocpath",       XP_RELLOCPATH},
    {"step",             XP_STEP},
    {"nodetest",         XP_NODE},
    {"nodetest fn",      XP_NODE_FN},
    {"predicates",       XP_PRED},
    {"primaryexpr",      XP_PRI0},
    {"primaryexpr nr",   XP_PRIME_NR},
    {"primaryexpr str",  XP_PRIME_STR},
    {"primaryexpr fn",   XP_PRIME_FN}, 
    {NULL,               -1}
};

/* Mapping between axis_type string <--> int  
 * @see axis_type_int2str
 */
static const map_str2int axis_type_map[] = {
    {"NaN",                 A_NAN},
    {"ancestor",            A_ANCESTOR},
    {"ancestor-or-self",    A_ANCESTOR_OR_SELF},
    {"attribute",           A_ATTRIBUTE},
    {"child",               A_CHILD},
    {"descendant",          A_DESCENDANT},
    {"descendant-or-self",  A_DESCENDANT_OR_SELF},
    {"following",           A_FOLLOWING},
    {"following-sibling",   A_FOLLOWING_SIBLING},
    {"namespace",           A_NAMESPACE},
    {"parent",              A_PARENT},
    {"preceding",           A_PRECEDING},
    {"preceding-sibling",   A_PRECEDING_SIBLING},
    {"self",                A_SELF},
    {"root",                A_ROOT},
    {NULL,                  -1}
};


/*
 * XPATH parse tree type
 */

/*! Map from axis-type int to string
 */
char*
axis_type_int2str(int axis_type)
{
    return (char*)clicon_int2str(axis_type_map, axis_type);
}


/*! Map from xpath_tree node name int to string
 */
char*
xpath_tree_int2str(int nodetype)
{
    return (char*)clicon_int2str(xpath_tree_map, nodetype);
}

/*! Print XPATH parse tree */
static int
xpath_tree_print0(cbuf       *cb,
		  xpath_tree *xs,
		  int         level)
{
    cprintf(cb, "%*s%s:", level*3, "", xpath_tree_int2str(xs->xs_type));
    if (xs->xs_s0)
	cprintf(cb, "\"%s\" ", xs->xs_s0);
    if (xs->xs_s1)
	cprintf(cb,"\"%s\" ", xs->xs_s1);
    if (xs->xs_int)
	switch (xs->xs_type){
	case XP_STEP:
	    cprintf(cb, "%s", axis_type_int2str(xs->xs_int));
	    break;
	default:
	    cprintf(cb, "%d ", xs->xs_int);
	    break;
	}
    if (xs->xs_strnr)
	cprintf(cb,"%s ", xs->xs_strnr);
    cprintf(cb, "\n");
    if (xs->xs_c0)
	xpath_tree_print0(cb, xs->xs_c0,level+1);
    if (xs->xs_c1)
	xpath_tree_print0(cb, xs->xs_c1, level+1);
    return 0;
}

/*! Print a xpath_tree to CLIgen buf
 * @param[out] cb  CLIgen buffer
 * @param[in]  xs  XPATH tree
 */
int
xpath_tree_print_cb(cbuf       *cb,
		    xpath_tree *xs)
{
    xpath_tree_print0(cb, xs, 0);
    return 0;
}

/*! Print a xpath_tree
 * @param[in]  f   UNIX output stream
 * @param[in]  xs  XPATH tree
 * @see xpath_tree2str
 */
int
xpath_tree_print(FILE       *f, 
		 xpath_tree *xs)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    if (xpath_tree_print0(cb, xs, 0) < 0)
	goto done;
    fprintf(f, "%s", cbuf_get(cb));
    retval = 0;
 done:
    return retval;
}

/*! Create an xpath string from an xpath tree, ie "unparsing"
 * @param[in]  xs    XPATH tree
 * @param[out] xpath XPath string as CLIgen buf
 * @see xpath_tree_print
 */
int
xpath_tree2cbuf(xpath_tree *xs,
		cbuf       *xcb)
{
    int   retval = -1;

    /* 1. Before first child */
    switch (xs->xs_type){
    case XP_ABSPATH:
	if (xs->xs_int == A_DESCENDANT_OR_SELF)
	    cprintf(xcb, "/");
	cprintf(xcb, "/");
	break;
    case XP_STEP:
	switch (xs->xs_int){
	case A_SELF:
	    cprintf(xcb, ".");
	    break;
	case A_PARENT:
	    cprintf(xcb, "..");
	    break;
	default:
	    break;
	}
	break;
    case XP_NODE: /* s0 is namespace prefix, s1 is name */
	if (xs->xs_s0)
	    cprintf(xcb, "%s:", xs->xs_s0);
	cprintf(xcb, "%s", xs->xs_s1);
	break;
    case XP_PRIME_NR:
	cprintf(xcb, "%s", xs->xs_strnr?xs->xs_strnr:"0"); 
	break;
    case XP_PRIME_STR:
	cprintf(xcb, "'%s'", xs->xs_s0?xs->xs_s0:"");
	break;
    case XP_PRIME_FN:
	if (xs->xs_s0)
	    cprintf(xcb, "%s(", xs->xs_s0);
	break;
    default:
	break;
    }
    /* 2. First child */
    if (xs->xs_c0 && xpath_tree2cbuf(xs->xs_c0, xcb) < 0)
	goto done;
    /* 3. Between first and second child */
    switch (xs->xs_type){
    case XP_AND: /* and or */
    case XP_ADD: /* div mod + * - */
	if (xs->xs_c1)
	    cprintf(xcb, " %s ", clicon_int2str(xpopmap, xs->xs_int));
	break;
    case XP_RELEX: /* !=, >= <= < > = */
    case XP_UNION: /* | */
	if (xs->xs_c1)
	    cprintf(xcb, "%s", clicon_int2str(xpopmap, xs->xs_int));
	break;
    case XP_PATHEXPR:
	/* [19]    PathExpr ::=  | FilterExpr '/' RelativeLocationPath	
		                 | FilterExpr '//' RelativeLocationPath	
	*/
	if (xs->xs_s0)
	    cprintf(xcb, "%s", xs->xs_s0);
	break;
    case XP_RELLOCPATH:
	if (xs->xs_c1){
	    if (xs->xs_int == A_DESCENDANT_OR_SELF)
		cprintf(xcb, "/");
	    cprintf(xcb, "/");
	}
	break;
    case XP_PRED:
	if (xs->xs_c1)
	    cprintf(xcb, "[");
	break;
    case XP_EXP:
	if (xs->xs_c0 && xs->xs_c1) /* Function name and two arguments, insert , */
	    cprintf(xcb, ",");
	break;
    default:
	break;
    }
    /* 4. Second child */
    if (xs->xs_c1 && xpath_tree2cbuf(xs->xs_c1, xcb) < 0)
	goto done;
    /* 5. After second child */
    switch (xs->xs_type){
    case XP_PRED:
	if (xs->xs_c1)
	    cprintf(xcb, "]");
	break;
    case XP_PRIME_FN:
	if (xs->xs_s0)
	    cprintf(xcb, ")");
    default:
	break;
    }
    retval = 0;
 done:
    return retval;
}

static int
xpath_tree_append(xpath_tree   *xt, 
		  xpath_tree ***vec, 
		  size_t       *len)
{
    int retval = -1;

    if ((*vec = realloc(*vec, sizeof(xpath_tree *) * (*len+1))) == NULL){
	clicon_err(OE_XML, errno, "realloc");
	goto done;
    }
    (*vec)[(*len)++] = xt;
    retval = 0;
 done:
    return retval;
}

/*! Check if two xpath-trees (parsed xpaths) ar equal
 *
 * @param[in]     xt1  XPath parse 1
 * @param[in]     xt2  XPath parse 2
 * @param[in,out] vec  XPath tree vector
 * @param[in,out] len  Length of XML XPath vector
 * @retval       -1    Error
 * @retval        0    Not equal
 * @retval        1    Equal
 */
int
xpath_tree_eq(xpath_tree   *xt1, /* pattern */
	      xpath_tree   *xt2,
	      xpath_tree ***vec, 
	      size_t       *len)
{
    int         retval = -1; /* Error */
    xpath_tree *xc1;
    xpath_tree *xc2;
    int         ret;

    /* node type */
    if (xt1->xs_type != xt2->xs_type
#if 1 /* special case that they are expressions but of different types */
	&& !((xt1->xs_type == XP_PRIME_NR || xt1->xs_type == XP_PRIME_STR) &&
	     (xt2->xs_type == XP_PRIME_NR || xt2->xs_type == XP_PRIME_STR))
#endif
	){
	clicon_debug(2, "%s type %s vs %s\n", __FUNCTION__,
		xpath_tree_int2str(xt1->xs_type),
		xpath_tree_int2str(xt2->xs_type));
	goto neq;
    }
    /* check match, if set, store and go directly to ok */
    if (xt1->xs_match){
	if (xpath_tree_append(xt2, vec, len) < 0)
	    goto done;
	goto eq;
    }
    if (xt1->xs_int != xt2->xs_int){
	clicon_debug(2, "%s int\n", __FUNCTION__);
	goto neq;
    }
    if (xt1->xs_double != xt2->xs_double){
	clicon_debug(2, "%s double\n", __FUNCTION__);
	goto neq;
    }
    if (clicon_strcmp(xt1->xs_s0, xt2->xs_s0)){
	clicon_debug(2, "%s s0\n", __FUNCTION__);
	goto neq;
    }
    if (clicon_strcmp(xt1->xs_s1, xt2->xs_s1)){
	clicon_debug(2, "%s s1\n", __FUNCTION__);
	goto neq;
    }
    xc1 = xt1->xs_c0;
    xc2 = xt2->xs_c0;
    if (xc1 == NULL && xc2 == NULL)
	;
    else{
	if (xc1 == NULL || xc2 == NULL){
	    clicon_debug(2, "%s NULL\n", __FUNCTION__);
	    goto neq;
	}
	if ((ret = xpath_tree_eq(xc1, xc2, vec, len)) < 0)
	    goto done;
	if (ret == 0)
	    goto neq;
    }
    xc1 = xt1->xs_c1;
    xc2 = xt2->xs_c1;
    if (xc1 == NULL && xc2 == NULL)
	;
    else{
	if (xc1 == NULL || xc2 == NULL){
	    clicon_debug(2, "%s NULL\n", __FUNCTION__);
	    goto neq;
	}
	if ((ret = xpath_tree_eq(xc1, xc2, vec, len)) < 0)
	    goto done;
	if (ret == 0)
	    goto neq;
    }
 eq:
    retval = 1; /* equal */
 done:
    return retval;
 neq:
    retval = 0; /* not equal */
    goto done;
}

/*! Traverse through an xpath-tree using indexes
 * @param[in]  xt  Start-tree
 * @param[in]  i   List of indexes terminated by -1: 0 means c0, 1 means c1
 * @retval     xc  End-tree
 */
xpath_tree *
xpath_tree_traverse(xpath_tree *xt,
		    ...)
{
    va_list     ap;
    int         i;
    xpath_tree *xs = xt;

    va_start(ap, xt);
    for (i = va_arg(ap, int); i >= 0; i = va_arg(ap, int)){
	switch (i){
	case 0:
	    xs = xs->xs_c0;
	    break;
	case 1:
	    xs = xs->xs_c1;
	    break;
	default:
	    break;
	}
    }
    va_end(ap);
    return xs;
}

/*! Free a xpath_tree
 * @param[in]  xs  XPATH tree
 * @see xpath_parse  creates a xpath_tree
 */
int
xpath_tree_free(xpath_tree *xs)
{
    if (xs->xs_strnr)
	free(xs->xs_strnr);
    if (xs->xs_s0)
	free(xs->xs_s0);
    if (xs->xs_s1)
	free(xs->xs_s1);
    if (xs->xs_c0)
	xpath_tree_free(xs->xs_c0);
    if (xs->xs_c1)
	xpath_tree_free(xs->xs_c1);
    free(xs);
    return 0;
}

/*! Given xpath, parse it, and return structured xpath tree 
 * @param[in]  xpath  String with XPATH 1.0 syntax
 * @param[out] xptree XPath-tree, parsed, structured XPATH, free:xpath_tree_free
 * @retval     0      OK
 * @retval    -1      Error
 * @code
 *   xpath_tree     *xpt = NULL;
 *   if (xpath_parse(xpath, &xpt) < 0)
 *     err;
 *   if (xpt)
 *	xpath_tree_free(xpt);
 * @endcode
 * @see xpath_tree_free 
 * @see xpath_tree2cbuf  for unparsing, ie producing an original xpath string
 */
int
xpath_parse(const char  *xpath,
	    xpath_tree **xptree)
{
    int               retval = -1;
    clixon_xpath_yacc xpy = {0,};
    cbuf             *cb = NULL;    

    if (xpath == NULL){
	clicon_err(OE_XML, EINVAL, "XPath is NULL");
	goto done;
    }
    xpy.xpy_parse_string = xpath;
    xpy.xpy_name = "xpath parser";
    xpy.xpy_linenum = 1;
    if (xpath_scan_init(&xpy) < 0)
	goto done;
    if (xpath_parse_init(&xpy) < 0)
	goto done;
    clicon_debug(2,"%s",__FUNCTION__);
    if (clixon_xpath_parseparse(&xpy) != 0) { /* yacc returns 1 on error */
	clicon_log(LOG_NOTICE, "XPATH error: on line %d", xpy.xpy_linenum);
	if (clicon_errno == 0)
	    clicon_err(OE_XML, 0, "XPATH parser error with no error code (should not happen)");
	xpath_scan_exit(&xpy);
	goto done;
    }
    if (clicon_debug_get() > 1){
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}
	xpath_tree_print_cb(cb, xpy.xpy_top);
	clicon_debug(2, "xpath parse tree:\n%s", cbuf_get(cb));
    }
    xpath_parse_exit(&xpy);
    xpath_scan_exit(&xpy);
    if (xptree){
	*xptree = xpy.xpy_top;
	xpy.xpy_top = NULL;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (xpy.xpy_top)
	xpath_tree_free(xpy.xpy_top);
    return retval;
}

/*! Given XML tree and xpath, parse xpath, eval it and return xpath context, 
 * This is a raw form of xpath where you can do type conversion of the return
 * value, etc, not just a nodeset.
 * @param[in]  xcur   XML-tree where to search
 * @param[in]  nsc    External XML namespace context, or NULL
 * @param[in]  xpath  String with XPATH 1.0 syntax
 * @param[in]  localonly Skip prefix and namespace tests (non-standard)
 * @param[out] xrp    Return XPATH context
 * @retval     0      OK
 * @retval    -1      Error
 * @code
 *   xp_ctx     *xc = NULL;
 *   if (xpath_vec_ctx(x, NULL, xpath, 0, &xc) < 0)
 *     err;
 *   if (xc)
 *	ctx_free(xc);
 * @endcode
 */
int
xpath_vec_ctx(cxobj      *xcur, 
	      cvec       *nsc,
	      const char *xpath,
	      int         localonly,
	      xp_ctx    **xrp)
{
    int         retval = -1;
    xpath_tree *xptree = NULL;
    xp_ctx      xc = {0,};
    
    if (xpath_parse(xpath, &xptree) < 0)
	goto done;
    xc.xc_type = XT_NODESET;
    xc.xc_node = xcur;
    xc.xc_initial = xcur;
    if (cxvec_append(xcur, &xc.xc_nodeset, &xc.xc_size) < 0)
	goto done;
    if (xp_eval(&xc, xptree, nsc, localonly, xrp) < 0)
	goto done;
    if (xc.xc_nodeset){
	free(xc.xc_nodeset);
	xc.xc_nodeset = NULL;
    }
    retval = 0;
 done:
    if (xptree)
	xpath_tree_free(xptree);
    return retval;
}

/*! XPath nodeset function where only the first matching entry is returned
 *
 * @param[in]  xcur      XML tree where to search
 * @param[in]  nsc       External XML namespace context, or NULL
 * @param[in]  xpformat  Format string for XPATH syntax
 * @retval     xml-tree  XML tree of first match
 * @retval     NULL      Error or not found
 *
 * @code
 *   cxobj *x;
 *   cvec  *nsc = NULL; // namespace context
 *   if (xml_nsctx_node(xtop, &nsc) < 0)
 *      err;
 *   if ((x = xpath_first(xtop, nsc, "//symbol/foo")) != NULL) {
 *         ...
 *   }
 * @endcode
 * @note  the returned pointer points into the original tree so should not be freed after use.
 * @note return value does not see difference between error and not found
 * @see also xpath_vec.
 */
cxobj *
xpath_first(cxobj      *xcur, 
	    cvec       *nsc,
	    const char *xpformat, 
	    ...)
{
    cxobj     *cx = NULL;
    va_list    ap;
    size_t     len;
    char      *xpath = NULL;
    xp_ctx    *xr = NULL;
    
    va_start(ap, xpformat);    
    len = vsnprintf(NULL, 0, xpformat, ap);
    va_end(ap);
    /* allocate a message string exactly fitting the message length */
    if ((xpath = malloc(len+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: compute write message from reason and args */
    va_start(ap, xpformat);    
    if (vsnprintf(xpath, len+1, xpformat, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    if (xpath_vec_ctx(xcur, nsc, xpath, 0, &xr) < 0)
	goto done;
    if (xr && xr->xc_type == XT_NODESET && xr->xc_size)
	cx = xr->xc_nodeset[0];
 done:
    if (xr)
	ctx_free(xr);
    if (xpath)
	free(xpath);
    return cx;
}

/*! XPath nodeset function where prefixes are skipped, only first matching is returned
 *
 * Reason for skipping prefix/namespace check may be with incomplete tree, for example.
 * @param[in]  xcur      XML tree where to search
 * @param[in]  xpformat  Format string for XPATH syntax
 * @retval     xml-tree  XML tree of first match
 * @retval     NULL      Error or not found
 *
 * @code
 *   cxobj *x;
 *   cvec  *nsc; // namespace context
 *   if ((x = xpath_first_localonly(xtop, "//symbol/foo")) != NULL) {
 *         ...
 *   }
 * @endcode
 * @note  the returned pointer points into the original tree so should not be freed after use.
 * @note return value does not see difference between error and not found
 * @note Prefixes and namespaces are ignored so this is NOT according to standard
 * @see also xpath_first.
 */
cxobj *
xpath_first_localonly(cxobj      *xcur, 
		      const char *xpformat, 
		      ...)
{
    cxobj     *cx = NULL;
    va_list    ap;
    size_t     len;
    char      *xpath = NULL;
    xp_ctx    *xr = NULL;
    
    va_start(ap, xpformat);    
    len = vsnprintf(NULL, 0, xpformat, ap);
    va_end(ap);
    /* allocate a message string exactly fitting the message length */
    if ((xpath = malloc(len+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: compute write message from reason and args */
    va_start(ap, xpformat);    
    if (vsnprintf(xpath, len+1, xpformat, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    if (xpath_vec_ctx(xcur, NULL, xpath, 1, &xr) < 0)
	goto done;
    if (xr && xr->xc_type == XT_NODESET && xr->xc_size)
	cx = xr->xc_nodeset[0];
 done:
    if (xr)
	ctx_free(xr);
    if (xpath)
	free(xpath);
    return cx;
}

/*! Given XML tree and xpath, returns nodeset as xml node vector
 * If result is not nodeset, return empty nodeset
 * @param[in]  xcur     xml-tree where to search
 * @param[in]  nsc      External XML namespace context, or NULL
 * @param[in]  xpformat Format string for XPATH syntax
 * @param[out] vec      vector of xml-trees. Vector must be free():d after use
 * @param[out] veclen   returns length of vector in return value
 * @retval     0        OK
 * @retval    -1        Error
 * @code
 *   cvec   *nsc; // namespace context
 *   cxobj **vec = NULL;
 *   size_t  veclen;
 *   if (xpath_vec(xcur, nsc, "//symbol/foo", &vec, &veclen) < 0) 
 *      err;
 *   for (i=0; i<veclen; i++){
 *      xn = vec[i];
 *         ...
 *   }
 *   free(vec);
 * @endcode
 */
int
xpath_vec(cxobj      *xcur, 
	  cvec       *nsc,
	  const char *xpformat, 
	  cxobj    ***vec, 
	  size_t     *veclen,
	  ...)
{
    int        retval = -1;
    va_list    ap;
    size_t     len;
    char      *xpath = NULL;
    xp_ctx    *xr = NULL; 
	
    va_start(ap, veclen);    
    len = vsnprintf(NULL, 0, xpformat, ap);
    va_end(ap);
    /* allocate an xpath string exactly fitting the length */
    if ((xpath = malloc(len+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: actually compute xpath string content */
    va_start(ap, veclen);    
    if (vsnprintf(xpath, len+1, xpformat, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    *vec=NULL;
    *veclen = 0;
    if (xpath_vec_ctx(xcur, nsc, xpath, 0, &xr) < 0)
	goto done;
    if (xr && xr->xc_type == XT_NODESET){
	*vec    = xr->xc_nodeset;
	xr->xc_nodeset = NULL;
	*veclen = xr->xc_size;
    }
    retval = 0;
 done:
    if (xr)
	ctx_free(xr);
    if (xpath)
	free(xpath);
    return retval;
}

/* XPath that returns a vector of matches (only nodes marked with flags)
 * @param[in]  xcur     xml-tree where to search
 * @param[in]  xpformat Format string for XPATH syntax
 * @param[in]  nsc      External XML namespace context, or NULL
 * @param[in]  flags    Set of flags that return nodes must match (0 if all)
 * @param[out] vec      vector of xml-trees. Vector must be free():d after use
 * @param[out] veclen   returns length of vector in return value
 * @retval     0        OK
 * @retval     -1       error.
 * @code
 *   cxobj **vec;
 *   int     veclen;
 *   cvec   *nsc; // namespace context (not NULL)
 *   if (xpath_vec_flag(xcur, nsc, "//symbol/foo", XML_FLAG_ADD, &vec, &veclen) < 0) 
 *      goto err;
 *   for (i=0; i<veclen; i++){
 *      xn = vec[i];
 *         ...
 *   }
 *   free(vec);
 * @endcode
 * @Note that although the returned vector must be freed after use, the returned xml trees need not be.
 * @see also xpath_vec  This is a specialized version.
 */
int
xpath_vec_flag(cxobj      *xcur, 
	       cvec       *nsc,
	       const char *xpformat, 
	       uint16_t    flags,
	       cxobj    ***vec, 
	       int        *veclen,
	       ...)
{
    int        retval = -1;
    va_list    ap;
    size_t     len;
    char      *xpath = NULL;
    xp_ctx    *xr = NULL;
    int        i;
    cxobj     *x;
    
    va_start(ap, veclen);    
    len = vsnprintf(NULL, 0, xpformat, ap);
    va_end(ap);
    /* allocate a message string exactly fitting the message length */
    if ((xpath = malloc(len+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: compute write message from reason and args */
    va_start(ap, veclen);    
    if (vsnprintf(xpath, len+1, xpformat, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    *vec=NULL;
    *veclen = 0;
    if (xpath_vec_ctx(xcur, nsc, xpath, 0, &xr) < 0)
	goto done;
    if (xr && xr->xc_type == XT_NODESET){
	for (i=0; i<xr->xc_size; i++){
	    x = xr->xc_nodeset[i];
	    if (flags==0x0 || xml_flag(x, flags))
		if (cxvec_append(x, vec, veclen) < 0)
		    goto done;		
	}
    }
    retval = 0;
 done:
    if (xr)
	ctx_free(xr);
    if (xpath)
	free(xpath);
    return retval;
}

/*! Given XML tree and xpath, returns boolean
 * Returns true if the nodeset is non-empty
 * @param[in]  xcur     xml-tree where to search
 * @param[in]  nsc      External XML namespace context, or NULL
 * @param[in]  xpformat Format string for XPATH syntax
 * @retval     1        True
 * @retval     0        False
 * @retval    -1        Error
 */
int
xpath_vec_bool(cxobj      *xcur, 
	       cvec       *nsc,
	       const char *xpformat, 
	       ...)
{
    int        retval = -1;
    va_list    ap;
    size_t     len;
    char      *xpath = NULL;
    xp_ctx    *xr = NULL;
    
    va_start(ap, xpformat);    
    len = vsnprintf(NULL, 0, xpformat, ap);
    va_end(ap);
    /* allocate a message string exactly fitting the message length */
    if ((xpath = malloc(len+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: compute write message from reason and args */
    va_start(ap, xpformat);    
    if (vsnprintf(xpath, len+1, xpformat, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    if (xpath_vec_ctx(xcur, nsc, xpath, 0, &xr) < 0)
	goto done;
    if (xr)
	retval = ctx2boolean(xr);
 done:
    if (xr)
	ctx_free(xr);
    if (xpath)
	free(xpath);
    return retval;
}

/*! Translate an xpath/nsc pair to a "canonical" form using yang prefixes
 *
 * @param[in]  xs      Parsed xpath - xpath_tree
 * @param[in]  yspec   Yang spec containing all modules, associated with namespaces
 * @param[in]  nsc0    Input namespace context
 * @param[out] nsc1    Output namespace context. Free after use with xml_nsctx_free
 * @param[out] reason  Error reason if result is 0 - failed
 * @retval     1       OK with nsc1 containing the transformed nsc
 * @retval     0       XPath failure with reason set to why
 * @retval    -1       Fatal Error
 */
static int
traverse_canonical(xpath_tree *xs,
		   yang_stmt  *yspec,
		   cvec       *nsc0,
		   cvec       *nsc1,
		   cbuf      **reason)
{
    int        retval = -1;
    char      *prefix0;
    char      *prefix1;
    char      *namespace;
    yang_stmt *ymod;
    cbuf      *cb = NULL;
    int        ret;

    switch (xs->xs_type){
    case XP_NODE: /* s0 is namespace prefix, s1 is name */
	/* Nodetest = * needs no prefix */
	if (xs->xs_s1 && strcmp(xs->xs_s1, "*") == 0)
	    break;
	prefix0 = xs->xs_s0;
	if ((namespace = xml_nsctx_get(nsc0, prefix0)) == NULL){
	    if ((cb = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cb, "No namespace found for prefix: %s", prefix0);
	    if (reason)
		*reason = cb;
	    goto failed;
	}
	if ((ymod = yang_find_module_by_namespace(yspec, namespace)) == NULL){
	    if ((cb = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cb, "No modules found for namespace: %s", namespace);	    
	    if (reason)
		*reason = cb;
	    goto failed;
	}
	if ((prefix1 = yang_find_myprefix(ymod)) == NULL){
	    if ((cb = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cb, "No prefix found in module: %s", yang_argument_get(ymod));	    
	    if (reason)
		*reason = cb;
	    goto failed;
	}
	if (xml_nsctx_get(nsc1, prefix1) == NULL)
	    if (xml_nsctx_add(nsc1, prefix1, namespace) < 0)
		goto done;
	if (prefix0==NULL || strcmp(prefix0, prefix1) != 0){
	    if (xs->xs_s0)
		free(xs->xs_s0);
	    if ((xs->xs_s0 = strdup(prefix1)) == NULL){
		clicon_err(OE_UNIX, errno, "strdup");
		goto done;
	    }
	}
	break;
    default:
	break;
    }	
    if (xs->xs_c0){
	if ((ret = traverse_canonical(xs->xs_c0, yspec, nsc0, nsc1, reason)) < 0)
	    goto done;
	if (ret == 0)
	    goto failed;
    }
    if (xs->xs_c1){
	if ((ret = traverse_canonical(xs->xs_c1, yspec, nsc0, nsc1, reason)) < 0)
	    goto done;
	if (ret == 0)
	    goto failed;
    }	
    retval = 1;
 done:
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Translate an xpath/nsc pair to a "canonical" form using yang prefixes
 *
 * @param[in]  xpath0  Input xpath
 * @param[in]  nsc0    Input namespace context
 * @param[in]  yspec   Yang spec containing all modules, associated with namespaces
 * @param[out] xpath1  Output xpath. Free after use with free
 * @param[out] nsc1    Output namespace context. Free after use with xml_nsctx_free
 * @retval     1       OK, xpath1 and nsc1 allocated
 * @retval     0       XPath failure with reason set to why
 * @retval    -1       Fatal Error
 * Example: 
 *  Module A has prefix a and namespace urn:example:a and symbols x 
 *  Module B with prefix b and namespace urn:example:b and symbols y
 *  Then incoming:
 *    xpath0: /x/c:y
 *    nsc0:   NULL:"urn:example:a"; c:"urn:example:b"
 *  will be translated to:
 *    xpath1: /a:x/b:y
 *    nsc1:   a:"urn:example:a"; b:"urn:example:b"
 * @code
 *   char *xpath1 = NULL;
 *   cvec *nsc1 = NULL;
 *   cbuf *reason = NULL;
 *   if ((ret = xpath2canonical(xpath0, nsc0, yspec, &xpath1, &nsc1, &reason)) < 0)
 *     err;
 *   ...
 *   if (xpath1) free(xpath1);
 *   if (nsc1) xml_nsctx_free(nsc1);
 * @endcode
 */
int
xpath2canonical(const char *xpath0,
		cvec       *nsc0,
		yang_stmt  *yspec,
		char      **xpath1,
		cvec      **nsc1p,
		cbuf      **cbreason)
{
    int         retval = -1;
    xpath_tree *xpt = NULL;
    cvec       *nsc1 = NULL;
    cbuf       *xcb = NULL;
    int         ret;

    /* Parse input xpath into an xpath-tree */
    if (xpath_parse(xpath0, &xpt) < 0)
	goto done;
    /* Create new nsc */
     if ((nsc1 = xml_nsctx_init(NULL, NULL)) == NULL)
	 goto done;
    /* Traverse tree to find prefixes, transform them to canonical form and
     * create a canonical network namespace
     */
     if ((ret = traverse_canonical(xpt, yspec, nsc0, nsc1, cbreason)) < 0)
	goto done;
     if (ret == 0)
	 goto failed;
     /* Print tree with new prefixes */
     if ((xcb = cbuf_new()) == NULL){
	 clicon_err(OE_XML, errno, "cbuf_new");
	 goto done;
     }
     if (xpath_tree2cbuf(xpt, xcb) < 0)
	 goto done;
     if (xpath1){
	 if ((*xpath1 = strdup(cbuf_get(xcb))) == NULL){
	     clicon_err(OE_UNIX, errno, "strdup");
	     goto done;
	 }
     }
    if (nsc1p){
	*nsc1p = nsc1;
	nsc1 = NULL;
    }
    retval = 1;
 done:
    if (xcb)
	cbuf_free(xcb);
    if (nsc1)
	xml_nsctx_free(nsc1);
    if (xpt)
	xpath_tree_free(xpt);
    return retval;
 failed:
    retval = 0;
    goto done;
}

/*! Return a count(xpath)
 *
 * @param[in]  xcur     xml-tree where to search
 * @param[in]  nsc      External XML namespace context, or NULL
 * @param[in]  xpath    XPATH syntax
 * @param[oit] count    Nr of elements of xpath
 * @note This function is made for making optimizations in certain circumstances, such as a list
 */
int
xpath_count(cxobj      *xcur, 
	    cvec       *nsc,
	    const char *xpath,
	    uint32_t   *count)
{
    int     retval = -1;
    xp_ctx *xc = NULL;
    cbuf   *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "count(%s)", xpath);
    if (xpath_vec_ctx(xcur, nsc, cbuf_get(cb), 0, &xc) < 0)
	goto done;
    if (xc && xc->xc_type == XT_NUMBER && xc->xc_number != NAN)
	*count = (uint32_t)xc->xc_number;
    else
	*count = 0;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (xc)
	ctx_free(xc);
    return retval;
}
