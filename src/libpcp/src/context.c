/*
 * Copyright (c) 2012-2016 Red Hat.
 * Copyright (c) 2007-2008 Aconex.  All Rights Reserved.
 * Copyright (c) 1995-2002,2004,2006,2008 Silicon Graphics, Inc.  All Rights Reserved.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * Thread-safe notes
 *
 * curr_handle needs to be thread-private
 * curr_ctx needs to be thread-private
 *
 * contexts[], contexts_map[], contexts_len and last_handle are protected
 * from changes * using the local contexts_lock mutex.
 *
 * Ditto for back n_backoff, def_backoff[] and backoff[].
 *
 * The actual contexts (__pmContext) are protected by the (recursive)
 * c_lock mutex which is intialized in pmNewContext() and pmDupContext(),
 * then locked in __pmHandleToPtr() ... it is the responsibility of all
 * __pmHandleToPtr() callers to call PM_UNLOCK(ctxp->c_lock) when they
 * are finished with the context.
 */

#include "pmapi.h"
#include "impl.h"
#include "internal.h"
#include <string.h>

static __pmContext	**contexts;		/* array of context ptrs */
static int		contexts_len;		/* number of contexts */
static int		last_handle = -1;	/* last returned context handle */
/*
 * For handle x above the PMAPI, if the context is valid, then for some
 * j (<=0 and < contexts_len), contexts_map[j] == x, and the real
 * __pmContext is found via contexts[j]
 */
static int		*contexts_map;

/*
 * Special sentinals for contexts_map[] ...
 */
#define MAP_FREE	-1		/* contexts[i] can be reused */
#define MAP_TEARDOWN	-2		/* contexts[i] is being destroyed */

#ifdef PM_MULTI_THREAD
#ifdef HAVE___THREAD
/* using a gcc construct here to make curr_handle thread-private */
static __thread int	curr_handle = PM_CONTEXT_UNDEF;	/* current context # */
static __thread __pmContext	*curr_ctxp = NULL;	/* -> current __pmContext */
#endif
#else
static int		curr_handle = PM_CONTEXT_UNDEF;	/* current context # */
static __pmContext	*curr_ctxp = NULL;		/* -> current __pmContext */
#endif

static int		n_backoff;
static int		def_backoff[] = {5, 10, 20, 40, 80};
static int		*backoff;

#ifdef PM_MULTI_THREAD
static pthread_mutex_t	contexts_lock = PTHREAD_MUTEX_INITIALIZER;
#else
void			*contexts_lock;
#endif

#if defined(PM_MULTI_THREAD) && defined(PM_MULTI_THREAD_DEBUG)
/*
 * return true if lock == contexts_lock
 */
int
__pmIsContextsLock(void *lock)
{
    return lock == (void *)&contexts_lock;
}
#endif

/*
 * Given a handle above the PMAPI, do the mapping to the index (ctxnum)
 * of the matching contexts[] entry
 */
static int
map_handle_nolock(int handle)
{
    int		ctxnum = -1;
    int		i;

    for (i = 0; i < contexts_len; i++) {
	if (contexts_map[i] == handle && contexts_map[i] >= 0) {
	    if (contexts[i]->c_type != PM_CONTEXT_INIT) {
		ctxnum = i;
		break;
	    }
	}
    }
    return ctxnum;
}

static int
map_handle(int handle)
{
    ASSERT_IS_LOCKED(contexts_lock);

    return map_handle_nolock(handle);
}

static void
waitawhile(__pmPMCDCtl *ctl)
{
    ASSERT_IS_LOCKED(ctl->pc_lock);
    /*
     * after failure, compute delay before trying again ...
     */
    PM_LOCK(contexts_lock);
    if (n_backoff == 0) {
	char	*q;
	int	bad = 0;
	/* first time ... try for PMCD_RECONNECT_TIMEOUT from env */
	PM_LOCK(__pmLock_extcall);
	q = getenv("PMCD_RECONNECT_TIMEOUT");		/* THREADSAFE */
	if (q != NULL)
	    q = strdup(q);
	PM_UNLOCK(__pmLock_extcall);
	if (q != NULL) {
	    char	*pend;
	    char	*p;
	    int		val;

	    for (p = q; *p != '\0'; ) {
		val = (int)strtol(p, &pend, 10);
		if (val <= 0 || (*pend != ',' && *pend != '\0')) {
		    /* report error below, after contexts_lock released */
		    if (backoff != NULL)
			free(backoff);
		    n_backoff = 0;
		    bad = 1;
		    break;
		}
		if ((backoff = (int *)realloc(backoff, (n_backoff+1) * sizeof(backoff[0]))) == NULL) {
		    __pmNoMem("pmReconnectContext", (n_backoff+1) * sizeof(backoff[0]), PM_FATAL_ERR);
		}
		backoff[n_backoff++] = val;
		if (*pend == '\0')
		    break;
		p = &pend[1];
	    }
	}
	if (n_backoff == 0) {
	    /* use default */
	    n_backoff = 5;
	    backoff = def_backoff;
	}
	PM_UNLOCK(contexts_lock);
	if (bad) {
	    __pmNotifyErr(LOG_WARNING,
			 "pmReconnectContext: ignored bad PMCD_RECONNECT_TIMEOUT = '%s'\n",
			 q);
	}
	if (q != NULL)
	    free(q);
    }
    else
	PM_UNLOCK(contexts_lock);
    if (ctl->pc_timeout == 0)
	ctl->pc_timeout = 1;
    else if (ctl->pc_timeout < n_backoff)
	ctl->pc_timeout++;
    ctl->pc_again = time(NULL) + backoff[ctl->pc_timeout-1];
}

/*
 * On success, context is locked and caller should unlock it
 */
__pmContext *
__pmHandleToPtr(int handle)
{
    int		i;

    PM_LOCK(contexts_lock);
    for (i = 0; i < contexts_len; i++) {
	if (contexts_map[i] == handle && contexts_map[i] >= 0) {
	    if (contexts[i]->c_type != PM_CONTEXT_INIT) {
		__pmContext	*sts = contexts[i];
		/*
		 * Important Note:
		 *   Once c_lock is locked for _any_ context, the caller
		 *   cannot call into the routines here where contexts_lock
		 *   is acquired without first releasing the c_lock for all
		 *   contexts that are locked.
		 */
		PM_LOCK(sts->c_lock);
		PM_UNLOCK(contexts_lock);
		return sts;
	    }
	}
    }
    PM_UNLOCK(contexts_lock);
    return NULL;
}

int
__pmPtrToHandle(__pmContext *ctxp)
{
    return ctxp->c_handle;
}

/*
 * Determine the hostname associated with the given context.
 */
char *
pmGetContextHostName_r(int ctxid, char *buf, int buflen)
{
    __pmContext *ctxp;
    char	*name;
    pmID	pmid;
    pmResult	*resp;
    int		save_handle;
    __pmContext	*save_ctxp;
    int		sts;

    PM_INIT_LOCKS();

    buf[0] = '\0';

    if ((ctxp = __pmHandleToPtr(ctxid)) != NULL) {
	switch (ctxp->c_type) {
	case PM_CONTEXT_HOST:
	    /*
	     * Try and establish the hostname from PMCD (possibly remote).
	     * Do not nest the successive actions. That way, if any one of
	     * them fails, we take the default.
	     * Note: we must *temporarily* switch context (see pmUseContext)
	     * in the host case, then switch back afterward. We already hold
	     * locks and have validated the context pointer, so we do a mini
	     * context switch, then switch back.
	     */
	    if (pmDebug & DBG_TRACE_CONTEXT)
		fprintf(stderr, "pmGetContextHostName_r context(%d) -> 0\n", ctxid);
	    save_handle = PM_TPD(curr_handle);
	    save_ctxp = PM_TPD(curr_ctxp);
	    PM_TPD(curr_handle) = ctxid;
	    PM_TPD(curr_ctxp) = ctxp;

	    name = "pmcd.hostname";
	    sts = pmLookupName(1, &name, &pmid);
	    if (sts >= 0)
		sts = pmFetch(1, &pmid, &resp);
	    if (pmDebug & DBG_TRACE_CONTEXT)
		fprintf(stderr, "pmGetContextHostName_r reset(%d) -> 0\n", save_handle);

	    PM_TPD(curr_handle) = save_handle;
	    PM_TPD(curr_ctxp) = save_ctxp;
	    if (sts >= 0) {
		if (resp->vset[0]->numval > 0) { /* pmcd.hostname present */
		    strncpy(buf, resp->vset[0]->vlist[0].value.pval->vbuf, buflen);
		    pmFreeResult(resp);
		    break;
		}
		pmFreeResult(resp);
		/* FALLTHROUGH */
	    }

	    /*
	     * We could not get the hostname from PMCD.  If the name in the
	     * context structure is a filesystem path (AF_UNIX address) or
	     * 'localhost', then use gethostname(). Otherwise, use the name
	     * from the context structure.
	     */
	    PM_LOCK(ctxp->c_pmcd->pc_lock);
	    name = ctxp->c_pmcd->pc_hosts[0].name;
	    if (!name || name[0] == __pmPathSeparator() || /* AF_UNIX */
		(strncmp(name, "localhost", 9) == 0)) /* localhost[46] */
		gethostname(buf, buflen);
	    else
		strncpy(buf, name, buflen-1);
	    PM_UNLOCK(ctxp->c_pmcd->pc_lock);
	    break;

	case PM_CONTEXT_LOCAL:
	    gethostname(buf, buflen);
	    break;

	case PM_CONTEXT_ARCHIVE:
	    strncpy(buf, ctxp->c_archctl->ac_log->l_label.ill_hostname, buflen-1);
	    break;
	}

	buf[buflen-1] = '\0';
	PM_UNLOCK(ctxp->c_lock);
    }

    return buf;
}

/*
 * Backward-compatibility interface, non-thread-safe variant.
 */
const char *
pmGetContextHostName(int ctxid)
{
    static char	hostbuf[MAXHOSTNAMELEN];
    return (const char *)pmGetContextHostName_r(ctxid, hostbuf, (int)sizeof(hostbuf));
}

int
pmWhichContext(void)
{
    /*
     * return curr_handle, provided it is defined
     */
    int		sts;

    PM_INIT_LOCKS();

    if (PM_TPD(curr_handle) > PM_CONTEXT_UNDEF)
	sts = PM_TPD(curr_handle);
    else
	sts = PM_ERR_NOCONTEXT;

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmWhichContext() -> %d, cur=%d\n",
	    sts, PM_TPD(curr_handle));
#endif
    return sts;
}

__pmContext *
__pmCurrentContext(void)
{
    PM_INIT_LOCKS();

    return PM_TPD(curr_ctxp);
}

int
__pmConvertTimeout(int timeo)
{
    double tout_msec;

    switch (timeo) {
    case TIMEOUT_NEVER:
	return -1;

    case TIMEOUT_DEFAULT:
	tout_msec = __pmRequestTimeout() * 1000.0;
	break;

    case TIMEOUT_CONNECT:
	tout_msec = __pmConnectTimeout() * 1000.0;
	break;

    default:
	tout_msec = timeo * 1000.0;
	break;
    }

    return (int)tout_msec;
}

#ifdef PM_MULTI_THREAD
/*
 * Called with contexts_lock mutex held.
 */
static void
initcontextlock(pthread_mutex_t *lock)
{
    pthread_mutexattr_t	attr;
    int			sts;
    char		errmsg[PM_MAXERRMSGLEN];

    ASSERT_IS_LOCKED(contexts_lock);

    /*
     * Need context lock to be recursive as we sometimes call
     * __pmHandleToPtr() while the current context is already
     * locked
     */
    if ((sts = pthread_mutexattr_init(&attr)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "pmNewContext: "
		"context=%d lock pthread_mutexattr_init failed: %s",
		contexts_len-1, errmsg);
	exit(4);
    }
    if ((sts = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "pmNewContext: "
		"context=%d lock pthread_mutexattr_settype failed: %s",
		contexts_len-1, errmsg);
	exit(4);
    }
    if ((sts = pthread_mutex_init(lock, &attr)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "pmNewContext: "
		"context=%d lock pthread_mutex_init failed: %s",
		contexts_len-1, errmsg);
	exit(4);
    }
    pthread_mutexattr_destroy(&attr);
}

static void
initchannellock(pthread_mutex_t *lock)
{
    int		sts;
    char	errmsg[PM_MAXERRMSGLEN];

    if ((sts = pthread_mutex_init(lock, NULL)) != 0) {
	pmErrStr_r(-sts, errmsg, sizeof(errmsg));
	fprintf(stderr, "pmNewContext: "
		"context=%d pmcd channel lock pthread_mutex_init failed: %s",
		contexts_len, errmsg);
	exit(4);
    }
}

#if 0
/*
 * TODO ... would like to make this go away entirely, but need to
 * address issue in TODO comment in __pmPMCDCtlFree() first
 * ... also the empty macro defn in the #else block below
 */
static void
destroylock(pthread_mutex_t *lock, char *which)
{
    int		psts;
    char	errmsg[PM_MAXERRMSGLEN];

    if ((psts = pthread_mutex_destroy(lock)) != 0) {
	pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	fprintf(stderr, "pmDestroyContext: pthread_mutex_destroy(%s) failed: %s\n", which, errmsg);
	/*
	 * Most likely cause is the mutex still being locked ... this is a
	 * a library bug, but potentially recoverable ...
	 */
	while (PM_UNLOCK(lock) >= 0) {
	    fprintf(stderr, "pmDestroyContext: extra %s unlock?\n", which);
	}
	if ((psts = pthread_mutex_destroy(lock)) != 0) {
	    pmErrStr_r(-psts, errmsg, sizeof(errmsg));
	    fprintf(stderr, "pmDestroyContext: pthread_mutex_destroy(%s) failed second try: %s\n", which, errmsg);
	}
    }
}
#endif
#else
#define initcontextlock(x)	do { } while (1)
#define initchannellock(x)	do { } while (1)
#define destroylock(x,y)	do { } while (1)
#endif

static int
ctxlocal(__pmHashCtl *attrs)
{
    int sts;
    char *name = NULL;
    char *container = NULL;

    PM_LOCK(__pmLock_extcall);
    if ((container = getenv("PCP_CONTAINER")) != NULL) {	/* THREADSAFE */
	if ((name = strdup(container)) == NULL) {
	    PM_UNLOCK(__pmLock_extcall);
	    return -ENOMEM;
	}
	PM_UNLOCK(__pmLock_extcall);
	if ((sts = __pmHashAdd(PCP_ATTR_CONTAINER, (void *)name, attrs)) < 0) {
	    free(name);
	    return sts;
	}
    }
    else
	PM_UNLOCK(__pmLock_extcall);
    return 0;
}

static int
ctxflags(__pmHashCtl *attrs, int *flags)
{
    int sts;
    char *name = NULL;
    char *secure = NULL;
    char *container = NULL;
    __pmHashNode *node;

    if ((node = __pmHashSearch(PCP_ATTR_PROTOCOL, attrs)) != NULL) {
	if (strcmp((char *)node->data, "pcps") == 0) {
	    if ((node = __pmHashSearch(PCP_ATTR_SECURE, attrs)) != NULL)
		secure = (char *)node->data;
	    else
		secure = "enforce";
	}
    }

    PM_LOCK(__pmLock_extcall);
    if (!secure)
	secure = getenv("PCP_SECURE_SOCKETS");		/* THREADSAFE */

    if (secure) {
	if (secure[0] == '\0' ||
	   (strcmp(secure, "1")) == 0 ||
	   (strcmp(secure, "enforce")) == 0) {
	    *flags |= PM_CTXFLAG_SECURE;
	} else if (strcmp(secure, "relaxed") == 0) {
	    *flags |= PM_CTXFLAG_RELAXED;
	}
    }
    PM_UNLOCK(__pmLock_extcall);

    if (__pmHashSearch(PCP_ATTR_COMPRESS, attrs) != NULL)
	*flags |= PM_CTXFLAG_COMPRESS;

    if (__pmHashSearch(PCP_ATTR_USERAUTH, attrs) != NULL ||
	__pmHashSearch(PCP_ATTR_USERNAME, attrs) != NULL ||
	__pmHashSearch(PCP_ATTR_PASSWORD, attrs) != NULL ||
	__pmHashSearch(PCP_ATTR_METHOD, attrs) != NULL ||
	__pmHashSearch(PCP_ATTR_REALM, attrs) != NULL)
	*flags |= PM_CTXFLAG_AUTH;

    if (__pmHashSearch(PCP_ATTR_CONTAINER, attrs) != NULL)
	*flags |= PM_CTXFLAG_CONTAINER;
    else {
	PM_LOCK(__pmLock_extcall);
	container = getenv("PCP_CONTAINER");		/* THREADSAFE */
	if (container != NULL) {
	    if ((name = strdup(container)) == NULL) {
		PM_UNLOCK(__pmLock_extcall);
		return -ENOMEM;
	    }
	    PM_UNLOCK(__pmLock_extcall);
	    if ((sts = __pmHashAdd(PCP_ATTR_CONTAINER, (void *)name, attrs)) < 0) {
		free(name);
		return sts;
	    }
	    *flags |= PM_CTXFLAG_CONTAINER;
	}
	else
	    PM_UNLOCK(__pmLock_extcall);
    }

    if (__pmHashSearch(PCP_ATTR_EXCLUSIVE, attrs) != NULL)
	*flags |= PM_CTXFLAG_EXCLUSIVE;

    return 0;
}

static int
ping_pmcd(int ctxnum, int handle, __pmPMCDCtl *pmcd)
{
    __pmPDU	*pb;
    int		sts, pinpdu;

    ASSERT_IS_LOCKED(pmcd->pc_lock);

    /*
     * We're going to leverage an existing host context, just make sure
     * pmcd is still alive at the other end ... we don't have a "ping"
     * PDU, but sending a pmDesc request for PM_ID_NULL is pretty much
     * the same thing ... expect a PM_ERR_PMID error PDU back.
     * The code here is based on pmLookupDesc() with some short cuts
     * because we know it is a host context and we already hold the
     * contexts_lock mutex.
     */

    if ((sts = __pmSendDescReq(pmcd->pc_fd, handle, PM_ID_NULL)) >= 0) {
	pinpdu = __pmGetPDU(pmcd->pc_fd, ANY_SIZE, pmcd->pc_tout_sec, &pb);
	if (pinpdu == PDU_ERROR)
	    __pmDecodeError(pb, &sts);
	else {
	    /* wrong PDU type or PM_ERR_* ... close channel to pmcd */
	    __pmCloseChannelbyContext(contexts[ctxnum], PDU_ERROR, pinpdu);
	}
	if (pinpdu > 0)
	    __pmUnpinPDUBuf(pb);
    }

    if (sts != PM_ERR_PMID) {
	/* pmcd is not well on this context ... */
	return 0;
    }
    return 1;
}


int
__pmFindOrOpenArchive(__pmContext *ctxp, const char *name, int multi_arch)
{
    __pmArchCtl *acp;
    __pmLogCtl	*lcp;
    __pmContext	*ctxp2;
    __pmArchCtl *acp2;
    __pmLogCtl	*lcp2;
    int		i;
    int		sts;

    PM_INIT_LOCKS();

    /*
     * We're done with the current archive, if any. Close it, if necessary.
     * If we do close it, keep the __pmLogCtl block for re-use of the l_pmns.
     */
    acp = ctxp->c_archctl;
    lcp = acp->ac_log;
    if (lcp) {
	if (--lcp->l_refcnt == 0)
	    __pmLogClose(lcp);
	else
	    lcp = NULL;
    }

    /*
     * See if an archive by this name is already open in another context.
     * We can't share archive control structs for multi-archive contexts
     * because, for those, there is a global l_pmns which is shared among the
     * archives in the context.
     *
     * We must take the contexts_lock mutex for this search.
     */
    PM_LOCK(contexts_lock);
    lcp2 = NULL;
    if (! multi_arch) {
	for (i = 0; i < contexts_len; i++) {
	    if (i == PM_TPD(curr_handle))
		continue;
	    if (contexts_map[i] < 0)
		continue;

	    /*
	     * See if there is already an archive opened with this name and
	     * not part of a multi-archive context.
	     */
	    ctxp2 = contexts[i];
	    if (ctxp2->c_type == PM_CONTEXT_ARCHIVE) {
		acp2 = ctxp2->c_archctl;
		if (! acp2->ac_log->l_multi &&
		    strcmp (name, acp2->ac_log->l_name) == 0) {
		    lcp2 = acp2->ac_log;
		    break;
		}
	    }
	}

	/*
	 * If we found an active archive with the same name, then use it.
	 * Free the current archive controls, if necessary.
	 */
	if (lcp2 != NULL) {
	    if (lcp)
		free(lcp);
	    ++lcp2->l_refcnt;
	    acp->ac_log = lcp2;
	    PM_UNLOCK(contexts_lock);
	    return 0;
	}
    }
    PM_UNLOCK(contexts_lock);

    /*
     * No usable, active archive with this name was found. Open one.
     * Allocate a new log control block, if necessary.
     */
    if (lcp == NULL) {
	if ((lcp = (__pmLogCtl *)malloc(sizeof(*lcp))) == NULL)
	    __pmNoMem("__pmFindOrOpenArchive", sizeof(*lcp), PM_FATAL_ERR);
	lcp->l_pmns = NULL;
	lcp->l_hashpmid.nodes = lcp->l_hashpmid.hsize = 0;
	lcp->l_hashindom.nodes = lcp->l_hashindom.hsize = 0;
	lcp->l_multi = multi_arch;
	acp->ac_log = lcp;
    }
    sts = __pmLogOpen(name, ctxp);
    if (sts < 0) {
	free(lcp);
	acp->ac_log = NULL;
    }
    else
	lcp->l_refcnt = 1;

    return sts;
}

static char *
addName(
  const char *dirname,
  char *list,
  size_t *listsize,
  const char *item,
  size_t itemsize
) {
    size_t dirsize;

    /* Was there a directory specified? */
    if (dirname != NULL )
	dirsize = strlen(dirname) + 1; /* room for the path separator */
    else
	dirsize = 0;

    /* Allocate more space */
    if (list == NULL) {
	if ((list = malloc(dirsize + itemsize + 1)) == NULL)
	    __pmNoMem("initArchive", itemsize + 1, PM_FATAL_ERR);
	*listsize = 0;
    }
    else {
	/* The comma goes where the previous nul was */
	if ((list = realloc(list, dirsize + *listsize + itemsize + 1)) == NULL)
	    __pmNoMem("initArchive", *listsize + itemsize + 1, PM_FATAL_ERR);
	list[*listsize - 1] = ',';
    }

    /* Add the new name */
    if (dirname != NULL) {
	strcpy(list + *listsize, dirname);
	*listsize += dirsize;
	list[*listsize - 1] = __pmPathSeparator();
    }
    memcpy(list + *listsize, item, itemsize);
    *listsize += itemsize + 1;
    list[*listsize - 1] = '\0';
    return list;
}

/*
 * The list of names may contain one or more directories. Examine the
 * list and replace the directories with the archives contained within.
 */
static char *
expandArchiveList(const char *names)
{
    const char	*current;
    const char	*end;
    size_t	length = 0;
    char	*newlist = NULL;
    size_t	newlistsize = 0;
    char	*dirname;
    const char	*suffix;
    DIR		*dirp = NULL;
#if defined(HAVE_READDIR64)
    struct dirent64	*direntp;
#else
    struct dirent	*direntp;
#endif
 
    current = names;
    while (*current) {
	/* Find the end of the current archive name. */
	end = strchr(current, ',');
	if (end)
	    length = end - current;
	else
	    length = strlen (current);

	/*
	 * If newname specifies a directory, then add each archive in the
	 * directory. Use opendir(3) directly instead of stat(3) or fstat(3) 
	 * in order to avoid a TOCTOU race between checking and opening the
	 * directory.
	 * We need nul terminated copy of the name fpr opendir(3).
	 */
	if ((dirname = malloc(length + 1)) == NULL)
	    __pmNoMem("initArchive", length + 1, PM_FATAL_ERR);
	memcpy(dirname, current, length);
	dirname[length] = '\0';

	/* dirp is an on-stack variable, so readdir*() is THREADSAFE */
	if ((dirp = opendir(dirname)) != NULL) {
#if defined(HAVE_READDIR64)
	    while ((direntp = readdir64(dirp)) != NULL) {	/* THREADSAFE */
#else
	    while ((direntp = readdir(dirp)) != NULL) {		/* THREADSAFE */
#endif
		/*
		 * If this file is part of an archive, then add it.
		 * Look for names ending in .meta. These are unique to
		 * each archive.
		 */
		suffix = strrchr(direntp->d_name, '.');
		if (suffix == NULL || strcmp(suffix, ".meta") != 0)
		    continue;
		/*
		 * THREADSAFE because addName() acquires no locks (other than
		 * on the fatal __pmNoMem() paths)
		 */
		newlist = addName(dirname, newlist, &newlistsize,
				   direntp->d_name, suffix - direntp->d_name);
	    }
	    closedir(dirp);
	}
	else {
	    newlist = addName(NULL, newlist, &newlistsize, current, length);
	}
	free(dirname);

	/* Reset for the next iteration. */
	current += length;
	if (*current == ',')
	    ++current;
    }

    return newlist;
}

/*
 * Initialize the given archive(s) for this context.
 *
 * 'name' may be a single archive name or a list of archive names separated by
 * commas.
 *
 * Coming soon:
 * - name can be one or more glob expressions specifying the archives of
 *   interest.
 *
 * NB: no locks are being held at entry.
 */
static int
initarchive(__pmContext	*ctxp, const char *name)
{
    int			i;
    int			sts;
    char		*namelist = NULL;
    const char		*current;
    char		*end;
    __pmArchCtl		*acp;
    __pmMultiLogCtl	*mlcp = NULL;
    int			multi_arch = 0;
    int			ignore;
    double		tdiff;
    pmLogLabel		label;
    __pmTimeval		tmpTime;

    /*
     * Catch these early. Formerly caught by __pmLogLoadLabel(), but with
     * multi-archive support, things are more complex now.
     */
    if (name == NULL || *name == '\0')
	return PM_ERR_LOGFILE;

    /* Allocate the structure for overal control of the archive(s). */
    if ((ctxp->c_archctl = (__pmArchCtl *)malloc(sizeof(__pmArchCtl))) == NULL)
	__pmNoMem("initArchive", sizeof(__pmArchCtl), PM_FATAL_ERR);
    acp = ctxp->c_archctl;
    acp->ac_num_logs = 0;
    acp->ac_log_list = NULL;
    acp->ac_log = NULL;
    acp->ac_mark_done = 0;

    /*
     * The list of names may contain one or more directories. Examine the
     * list and replace the directories with the archives contained within.
     */
    if ((namelist = expandArchiveList(name)) == NULL) {
	sts = PM_ERR_LOGFILE;
	goto error;
    }

    /*
     * Initialize a __pmMultiLogCtl structure for each of the named archives.
     * sort them in order of start time and check for overlaps. Keep the final
     * archive open.
     */
    acp->ac_log_list = NULL;
    current = namelist;
    while (*current) {
	/* Find the end of the current archive name. */
	end = strchr(current, ',');
	if (end) {
	    multi_arch = 1;
	    *end = '\0';
	}

	/*
	 * Obtain a handle for the named archive.
	 * __pmFindOrOpenArchive() will take care of closing the active archive,
	 * if necessary
	 */
	sts = __pmFindOrOpenArchive(ctxp, current, multi_arch);
	if (sts < 0)
	    goto error;

	/*
	 * Obtain the start time of this archive. The end time could change
	 * on the fly and needs to be re-checked as needed.
	 */
	if ((sts = __pmGetArchiveLabel(ctxp->c_archctl->ac_log, &label)) < 0)
	    goto error;

	/*
	 * Insert this new entry into the list in sequence by time. Check for
	 * overlaps. Also check for duplicates.
	 */
	tmpTime.tv_sec = (__uint32_t)label.ll_start.tv_sec;
	tmpTime.tv_usec = (__uint32_t)label.ll_start.tv_usec;
	ignore = 0;
	for (i = 0; i < acp->ac_num_logs; i++) {
	    tdiff = __pmTimevalSub(&tmpTime, &acp->ac_log_list[i]->ml_starttime);
	    if (tdiff < 0.0) /* found insertion point */
		break;
	    if (tdiff == 0.0) {
		/* Is it a duplicate? */
		if (strcmp (current, acp->ac_log_list[i]->ml_name) == 0) {
		    ignore = 1;
		    break;
		}
		/* timespan overlap */
		sts = PM_ERR_LOGOVERLAP;
		goto error;
	    }
	    /* Keep checking */
	}

	if (! ignore) {
	    /* Initialize a new ac_log_list entry for this archive. */
	    acp->ac_log_list = realloc(acp->ac_log_list,
				       (acp->ac_num_logs + 1) *
				       sizeof(*acp->ac_log_list));
	    if (acp->ac_log_list == NULL) {
		__pmNoMem("initArchive",
			  (acp->ac_num_logs + 1) * sizeof(*acp->ac_log_list),
			  PM_FATAL_ERR);
	    }
	    if ((mlcp = (__pmMultiLogCtl *)malloc(sizeof(__pmMultiLogCtl))) == NULL)
		__pmNoMem("initArchive", sizeof(__pmMultiLogCtl), PM_FATAL_ERR);
	    if ((mlcp->ml_name = strdup(current)) == NULL)
		__pmNoMem("initArchive", strlen(current) + 1, PM_FATAL_ERR);
	    if ((mlcp->ml_hostname = strdup(label.ll_hostname)) == NULL)
		__pmNoMem("initArchive", strlen(label.ll_hostname) + 1, PM_FATAL_ERR);
	    if ((mlcp->ml_tz = strdup(label.ll_tz)) == NULL)
		__pmNoMem("initArchive", strlen(label.ll_tz) + 1, PM_FATAL_ERR);
	    mlcp->ml_starttime = tmpTime;

	    /*
	     * If we found the insertion point, then make room for the current
	     * archive in that slot. Otherwise, i refers to the end of the list,
	     * which is the correct slot.
	     */
	    if (i < acp->ac_num_logs) {
		memmove (&acp->ac_log_list[i + 1], &acp->ac_log_list[i],
			 (acp->ac_num_logs - i) * sizeof(*acp->ac_log_list));
	    }
	    acp->ac_log_list[i] = mlcp;
	    mlcp = NULL;
	    acp->ac_cur_log = acp->ac_num_logs;
	    ++acp->ac_num_logs;
	}

	/* Set up to process the next name. */
	if (! end)
	    break;
	current = end + 1;
    }
    free(namelist);
    namelist = NULL;

    if (acp->ac_num_logs > 1) {
	/*
	 * In order to maintain API semantics with the old single archive
	 * implementation, open the first archive and switch to the first volume.
	 */
	sts = __pmLogChangeArchive(ctxp, 0);
	if (sts < 0)
	    goto error;
	sts = __pmLogChangeVol(acp->ac_log, acp->ac_log->l_minvol);
	if (sts < 0)
	    goto error;
    }

    /* start after header + label record + trailer */
    ctxp->c_origin.tv_sec = (__int32_t)acp->ac_log->l_label.ill_start.tv_sec;
    ctxp->c_origin.tv_usec = (__int32_t)acp->ac_log->l_label.ill_start.tv_usec;
    ctxp->c_mode = (ctxp->c_mode & 0xffff0000) | PM_MODE_FORW;
    acp->ac_offset = sizeof(__pmLogLabel) + 2*sizeof(int);
    acp->ac_vol = acp->ac_log->l_curvol;
    acp->ac_serial = 0;		/* not serial access, yet */
    acp->ac_pmid_hc.nodes = 0;	/* empty hash list */
    acp->ac_pmid_hc.hsize = 0;
    acp->ac_end = 0.0;
    acp->ac_want = NULL;
    acp->ac_unbound = NULL;
    acp->ac_cache = NULL;

    return 0; /* success */

 error:
    if (mlcp) {
	if (mlcp->ml_name)
	    free (mlcp->ml_name);
	if (mlcp->ml_hostname)
	    free (mlcp->ml_hostname);
	if (mlcp->ml_tz)
	    free (mlcp->ml_tz);
	free(mlcp);
    }
    if (namelist)
	free(namelist);
    if (acp) {
	if (acp->ac_log_list) {
	    while (acp->ac_num_logs > 0) {
		--acp->ac_num_logs;
		if (acp->ac_log_list[acp->ac_num_logs]) {
		    free(acp->ac_log_list[acp->ac_num_logs]->ml_name);
		    free(acp->ac_log_list[acp->ac_num_logs]->ml_hostname);
		    free(acp->ac_log_list[acp->ac_num_logs]->ml_tz);
		    free(acp->ac_log_list[acp->ac_num_logs]);
		}
	    }
	    free(acp->ac_log_list);
	}
	if (acp->ac_log && --acp->ac_log->l_refcnt == 0)
	    free(acp->ac_log);
	free(acp);
    }
    return sts;
}

int
pmNewContext(int type, const char *name)
{
    __pmContext	*new = NULL;
    __pmContext	**list;
    int		*list_map;
    int		i;
    int		sts;
    int		old_curr_handle;
    __pmContext	*old_curr_ctxp;
    int		ctxnum;		/* index into contexts[] for new context */
    /* A pointer to this stub object is put in contexts[] while a real __pmContext is being built. */
    static /*const*/ __pmContext being_initialized = { .c_type = PM_CONTEXT_INIT };

    PM_INIT_LOCKS();

    if (PM_CONTEXT_LOCAL == (type & PM_CONTEXT_TYPEMASK) &&
	PM_MULTIPLE_THREADS(PM_SCOPE_DSO_PMDA))
	/* Local context requires single-threaded applications */
	return PM_ERR_THREAD;

    old_curr_handle = PM_TPD(curr_handle);
    old_curr_ctxp = PM_TPD(curr_ctxp);

    PM_LOCK(contexts_lock);
    /* See if we can reuse a free context */
    for (i = 0; i < contexts_len; i++) {
	if (contexts_map[i] == MAP_FREE) {
	    ctxnum = i;
	    new = contexts[ctxnum];
	    goto INIT_CONTEXT;
	}
    }

    /* Create a new one */
    if (contexts == NULL) {
	list = (__pmContext **)malloc(sizeof(__pmContext *));
	list_map = (int *)malloc(sizeof(int));
    }
    else {
	list = (__pmContext **)realloc((void *)contexts, (1+contexts_len) * sizeof(__pmContext *));
	list_map = (int *)realloc((void *)contexts_map, (1+contexts_len) * sizeof(int));
    }
    if (list == NULL || list_map == NULL) {
	sts = -oserror();
	goto FAILED_LOCKED;
    }
    contexts = list;
    contexts_map = list_map;
    /*
     * NB: it is harmless (not a leak) if contexts[] and/or contexts_map[]
     * is realloc'd a little larger, and then the last slot is not
     * initialized (since context_len is not incremented, and/or
     * initialization fails.
     * A subsequent pmNewContext allocation attempt will just do the
     * realloc again, and that time it'll be a trivial success.
     */

    new = (__pmContext *)malloc(sizeof(__pmContext));
    if (new == NULL) {
	sts = -oserror();
	goto FAILED_LOCKED;
    }
    memset(new, 0, sizeof(__pmContext));
    initcontextlock(&new->c_lock);

    ctxnum = contexts_len;
    contexts_len++;

    /*
     * We do not need to hold contexts_lock just for filling of the
     * new __pmContext structure.  This is good because archive and
     * remote-host setup operations can take centiseconds through
     * decaseconds of time.  We will need to re-lock to put the
     * initialized __pmContext into the contexts[] slot though
     * (e.g. for memory barrier purposes).
     */
INIT_CONTEXT:

    /*
     * Set up the default state
     */
    PM_TPD(curr_ctxp) = new;
    PM_TPD(curr_handle) = new->c_handle = ++last_handle;
    contexts[ctxnum] = &being_initialized;
    contexts_map[ctxnum] = last_handle;
    PM_UNLOCK(contexts_lock);
    /* c_lock not re-initialized, created once from initcontextlock() above */
    new->c_type = (type & PM_CONTEXT_TYPEMASK);
    new->c_mode = 0;
    new->c_origin.tv_sec = new->c_origin.tv_usec = 0;
    new->c_delta = 0;
    new->c_sent = 0;
    new->c_flags = (type & ~PM_CONTEXT_TYPEMASK);
    if ((new->c_instprof = (__pmProfile *)calloc(1, sizeof(__pmProfile))) == NULL) {
	/*
	 * fail : nothing changed -- actually list is changed, but restoring
	 * contexts_len should make it ok next time through
	 */
	sts = -oserror();
	goto FAILED;
    }
    new->c_instprof->state = PM_PROFILE_INCLUDE;	/* default global state */

    if (new->c_type == PM_CONTEXT_HOST) {
	__pmHashCtl	*attrs = &new->c_attrs;
	pmHostSpec	*hosts = NULL;
	int		nhosts;
	char		*errmsg;

	/* break down a host[:port@proxy:port][?attributes] specification */
	__pmHashInit(attrs);
	sts = __pmParseHostAttrsSpec(name, &hosts, &nhosts, attrs, &errmsg);
	if (sts < 0) {
	    pmprintf("pmNewContext: bad host specification\n%s", errmsg);
	    pmflush();
	    free(errmsg);
	    if (hosts != NULL)
		__pmFreeHostAttrsSpec(hosts, nhosts, attrs);
	    __pmHashClear(attrs);
	    goto FAILED;
	} else if (nhosts == 0) {
	    if (hosts != NULL)
		__pmFreeHostAttrsSpec(hosts, nhosts, attrs);
	    __pmHashClear(attrs);
	    sts = PM_ERR_NOTHOST;
	    goto FAILED;
	} else if ((sts = ctxflags(attrs, &new->c_flags)) < 0) {
	    if (hosts != NULL)
		__pmFreeHostAttrsSpec(hosts, nhosts, attrs);
	    __pmHashClear(attrs);
	    goto FAILED;
	}

	/*
	 * As an optimization, if there is already a connection to the
	 * same PMCD, we try to reuse (share) it.  This is not viable
	 * in several situations - when pmproxy is in use, or when any
	 * connection attribute(s) are set, or when exclusion has been
	 * explicitly requested (i.e. PM_CTXFLAG_EXCLUSIVE in c_flags).
	 * A reference count greater than one indicates active sharing.
	 *
	 * Note the detection of connection-to-same-pmcd is flawed, as
	 * hostname equality does not necessarily mean the connections
	 * are equal; e.g., the IP address might have changed.
	 *
	 * It is the topic of some debate as to whether PMCD connection
	 * sharing is of much value at all, especially considering the
	 * number of subtle and nasty bugs it has caused over time.  Do
	 * not rely on this behaviour, it may well be removed someday.
	 *
	 * NB: Take the contexts_lock mutex while we search the contexts[].
	 * This is not great, as the ping_pmcd() check can take some
	 * milliseconds, but it is necessary to avoid races between
	 * pmDestroyContext() and/or memory ordering.  For connections
	 * being shared, the refcnt is incremented under contexts_lock mutex.
	 * Decrementing refcnt occurs in pmDestroyContext while holding
	 * both the contexts_lock mutex and the c_lock mutex.
	 */
	PM_LOCK(contexts_lock);
	if (nhosts == 1) { /* not proxied */
	    for (i = 0; i < contexts_len; i++) {
		__pmPMCDCtl *pmcd;

		if (i == PM_TPD(curr_handle))
		    continue;
		if (contexts_map[i] < 0)
		    continue;
		pmcd = contexts[i]->c_pmcd;
		if (contexts_map[i] >= 0 &&
		    contexts[i]->c_type == new->c_type &&
		    contexts[i]->c_flags == new->c_flags &&
		    contexts[i]->c_flags == 0) {
		    PM_LOCK(pmcd->pc_lock);
		    if (strcmp(pmcd->pc_hosts[0].name, hosts[0].name) == 0 &&
			pmcd->pc_hosts[0].nports == hosts[0].nports) {
			int j, ports_same = 1;

			for (j = 0; j < hosts[0].nports; j++) {
			    if (pmcd->pc_hosts[0].ports[j] != hosts[0].ports[j]) {
				ports_same = 0;
				break;
			    }
			}

			/* ports match, check that pmcd is alive too */
			if (ports_same && ping_pmcd(i, contexts_map[i], pmcd)) {
			    new->c_pmcd = pmcd;
			    new->c_pmcd->pc_refcnt++;
			    PM_UNLOCK(pmcd->pc_lock);
			    break;
			}
		    }
		    PM_UNLOCK(pmcd->pc_lock);
		}
	    }
	}
	PM_UNLOCK(contexts_lock);

	if (new->c_pmcd == NULL) {
	    /*
	     * Try to establish the connection.
	     * If this fails, restore the original current context
	     * and return an error.  We unlock during the __pmConnectPMCD
             * to permit another pmNewContext to start during this time.
             * This is OK because this particular context won't be accessed
             * by valid code, except in the above search for shareable contexts.
             * But that code will reject it because our c_pmcd == NULL.
             */
            sts = __pmConnectPMCD(hosts, nhosts, new->c_flags, &new->c_attrs);
            if (sts < 0) {
		__pmFreeHostAttrsSpec(hosts, nhosts, attrs);
		__pmHashClear(attrs);
		goto FAILED;
	    }

	    new->c_pmcd = (__pmPMCDCtl *)calloc(1,sizeof(__pmPMCDCtl));
	    if (new->c_pmcd == NULL) {
		sts = -oserror();
		__pmCloseSocket(sts);
		__pmFreeHostAttrsSpec(hosts, nhosts, attrs);
		__pmHashClear(attrs);
		goto FAILED;
	    }
	    /*
	     * THREADSAFE - don't need pc_lock here as __pmPMCDCtl is not
	     * yet visible to other threads
	     */
	    new->c_pmcd->pc_fd = sts;
	    new->c_pmcd->pc_hosts = hosts;
	    new->c_pmcd->pc_nhosts = nhosts;
	    new->c_pmcd->pc_tout_sec = __pmConvertTimeout(TIMEOUT_DEFAULT) / 1000;
	    initchannellock(&new->c_pmcd->pc_lock);
            new->c_pmcd->pc_refcnt++;
	}
	else {
	    /* duplicate of an existing context, don't need the __pmHostSpec */
	    __pmFreeHostAttrsSpec(hosts, nhosts, attrs);
	    __pmHashClear(attrs);
	}
    }
    else if (new->c_type == PM_CONTEXT_LOCAL) {
	if ((sts = ctxlocal(&new->c_attrs)) != 0)
	    goto FAILED;
	if ((sts = __pmConnectLocal(&new->c_attrs)) != 0)
	    goto FAILED;
    }
    else if (new->c_type == PM_CONTEXT_ARCHIVE) {
        /*
         * Unlock during the archive inital file opens, which can take
         * a noticeable amount of time, esp. for multi-archives.  This
         * is OK because no other thread can validly touch our
         * partly-initialized context.
         */
        sts = initarchive(new, name);
        if (sts < 0)
	    goto FAILED;
    }
    else {
	/* bad type */
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT) {
	    fprintf(stderr, "pmNewContext(%d, %s): illegal type\n",
		    type, name);
	}
#endif
	return PM_ERR_NOCONTEXT;
    }

    /* Take contexts_lock mutex to update contexts[] with this fully operational
       battle station ^W context. */
    PM_LOCK(contexts_lock);
    contexts[ctxnum] = new;
    PM_UNLOCK(contexts_lock);

    /* return the handle to the new (current) context */
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT) {
	fprintf(stderr, "pmNewContext(%d, %s) -> %d\n", type, name, PM_TPD(curr_handle));
	__pmDumpContext(stderr, PM_TPD(curr_handle), PM_INDOM_NULL);
    }
#endif

    /* bind defined metrics if any ..., after the new context is in place */
    __dmopencontext(new);

    return PM_TPD(curr_handle);

FAILED:
    /*
     * We're contexts_lock unlocked at this stage.  We may have allocated a
     * __pmContext; we may have partially initialized it, but
     * something went wrong.  Let's install it as a blank
     * free entry in contexts[] to replace the PM_CONTEXT_INIT
     * stub we left in its place.
    */
    PM_LOCK(contexts_lock);

FAILED_LOCKED:
    if (new != NULL) {
	if (new->c_instprof != NULL) {
	    free(new->c_instprof);
            new->c_instprof = NULL;
        }
        /* We could memset-0 the struct, but this is not really
           necessary.  That's the first thing we'll do in INIT_CONTEXT. */
        contexts[ctxnum] = new;
	contexts_map[ctxnum] = MAP_FREE;
    }
    PM_TPD(curr_handle) = old_curr_handle;
    PM_TPD(curr_ctxp) = old_curr_ctxp;
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmNewContext(%d, %s) -> %d, curr_handle=%d\n",
	    type, name, sts, PM_TPD(curr_handle));
#endif
    PM_UNLOCK(contexts_lock);
    return sts;
}

int
pmReconnectContext(int handle)
{
    __pmContext	*ctxp;
    __pmPMCDCtl	*ctl;
    int		i, sts;
    int		ctxnum;

    /* NB: This function may need parallelization, to permit multiple threads
       to pmReconnectContext() concurrently.  __pmConnectPMCD can take multiple
       seconds while holding the contexts_lock mutex, bogging other context
       operations down. */
    PM_LOCK(contexts_lock);
    if ((ctxnum = map_handle(handle)) < 0) {
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT)
	    fprintf(stderr, "pmReconnectContext(%d) -> %d\n", handle, PM_ERR_NOCONTEXT);
#endif
	PM_UNLOCK(contexts_lock);
	return PM_ERR_NOCONTEXT;
    }

    ctxp = contexts[ctxnum];
    PM_LOCK(ctxp->c_lock);
    PM_UNLOCK(contexts_lock);
    ctl = ctxp->c_pmcd;
    if (ctxp->c_type == PM_CONTEXT_HOST) {
	PM_LOCK(ctl->pc_lock);
	if (ctl->pc_timeout && time(NULL) < ctl->pc_again) {
	    /* too soon to try again */
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT)
	    fprintf(stderr, "pmReconnectContext(%d) -> %d, too soon (need wait another %d secs)\n",
		handle, (int)-ETIMEDOUT, (int)(ctl->pc_again - time(NULL)));
#endif
	    PM_UNLOCK(ctl->pc_lock);
	    PM_UNLOCK(ctxp->c_lock);
	    return -ETIMEDOUT;
	}

	if (ctl->pc_fd >= 0) {
	    /* don't care if this fails */
	    __pmCloseSocket(ctl->pc_fd);
	    ctl->pc_fd = -1;
	}

	if ((sts = __pmConnectPMCD(ctl->pc_hosts, ctl->pc_nhosts,
				   ctxp->c_flags, &ctxp->c_attrs)) < 0) {
	    waitawhile(ctl);
#ifdef PCP_DEBUG
	    if (pmDebug & DBG_TRACE_CONTEXT)
		fprintf(stderr, "pmReconnectContext(%d), failed (wait %d secs before next attempt)\n",
		    handle, (int)(ctl->pc_again - time(NULL)));
#endif
	    PM_UNLOCK(ctl->pc_lock);
	    PM_UNLOCK(ctxp->c_lock);
	    return -ETIMEDOUT;
	}
	else {
	    ctl->pc_fd = sts;
	    ctl->pc_timeout = 0;
	    ctxp->c_sent = 0;

#ifdef PCP_DEBUG
	    if (pmDebug & DBG_TRACE_CONTEXT)
		fprintf(stderr, "pmReconnectContext(%d), done\n", handle);
#endif
	}
	PM_UNLOCK(ctl->pc_lock);
    }

    /* clear any derived metrics and re-bind */
    __dmclosecontext(ctxp);
    __dmopencontext(ctxp);
    PM_UNLOCK(ctxp->c_lock);

    if (ctxp->c_type == PM_CONTEXT_HOST) {
	/* mark profile as not sent for all contexts sharing this socket */
	PM_LOCK(contexts_lock);
	for (i = 0; i < contexts_len; i++) {
	    if (contexts_map[i] != MAP_FREE &&
		contexts_map[i] != MAP_TEARDOWN &&
		contexts[i]->c_type != PM_CONTEXT_INIT &&
		contexts[i]->c_pmcd == ctl) {
		contexts[i]->c_sent = 0;
	    }
	}
	PM_UNLOCK(contexts_lock);
    }

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmReconnectContext(%d) -> %d\n", handle, handle);
#endif

    return handle;
}

int
pmDupContext(void)
{
    int			sts, oldtype;
    int			old, new = -1;
    char		hostspec[4096];
    __pmContext		*newcon, *oldcon;
    __pmMultiLogCtl	*newmlcp, *oldmlcp;
    __pmInDomProfile	*q, *p, *p_end;
    int			i;
    int			ctxnum;

    if ((old = pmWhichContext()) < 0) {
	sts = old;
	goto done;
    }
    PM_LOCK(contexts_lock);
    if ((ctxnum = map_handle(old)) < 0) {
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT)
	    fprintf(stderr, "pmDupContext(%d) -> %d\n", old, PM_ERR_NOCONTEXT);
#endif
	PM_UNLOCK(contexts_lock);
	return PM_ERR_NOCONTEXT;
    }

    oldcon = contexts[ctxnum];
    PM_UNLOCK(contexts_lock);
    oldtype = oldcon->c_type | oldcon->c_flags;
    if (oldcon->c_type == PM_CONTEXT_HOST) {
	PM_LOCK(oldcon->c_pmcd->pc_lock);
	__pmUnparseHostSpec(oldcon->c_pmcd->pc_hosts,
			oldcon->c_pmcd->pc_nhosts, hostspec, sizeof(hostspec));
	PM_UNLOCK(oldcon->c_pmcd->pc_lock);
	new = pmNewContext(oldtype, hostspec);
    }
    else if (oldcon->c_type == PM_CONTEXT_LOCAL)
	new = pmNewContext(oldtype, NULL);
    else if (oldcon->c_type == PM_CONTEXT_ARCHIVE)
	new = pmNewContext(oldtype, oldcon->c_archctl->ac_log->l_name);
    if (new < 0) {
	/* failed to connect or out of memory */
	sts = new;
	goto done;
    }
    PM_LOCK(contexts_lock);
    if ((ctxnum = map_handle(new)) < 0) {
	sts = PM_ERR_NOCONTEXT;
	PM_UNLOCK(contexts_lock);
	goto done;
    }
    newcon = contexts[ctxnum];
    PM_LOCK(oldcon->c_lock);
    PM_LOCK(newcon->c_lock);
    PM_UNLOCK(contexts_lock);
    /*
     * cherry-pick the fields of __pmContext that need to be copied
     */
    newcon->c_mode = oldcon->c_mode;
    newcon->c_pmcd = oldcon->c_pmcd;
    newcon->c_origin = oldcon->c_origin;
    newcon->c_delta = oldcon->c_delta;
    newcon->c_flags = oldcon->c_flags;

    /* clone the per-domain profiles (if any) */
    if (oldcon->c_instprof->profile_len > 0) {
	newcon->c_instprof->profile = (__pmInDomProfile *)malloc(
	    oldcon->c_instprof->profile_len * sizeof(__pmInDomProfile));
	if (newcon->c_instprof->profile == NULL) {
	    sts = -oserror();
	    goto done_locked;
	}
	memcpy(newcon->c_instprof->profile, oldcon->c_instprof->profile,
	    oldcon->c_instprof->profile_len * sizeof(__pmInDomProfile));
	p = oldcon->c_instprof->profile;
	p_end = p + oldcon->c_instprof->profile_len;
	q = newcon->c_instprof->profile;
	for (; p < p_end; p++, q++) {
	    if (p->instances) {
		q->instances = (int *)malloc(p->instances_len * sizeof(int));
		if (q->instances == NULL) {
		    sts = -oserror();
		    goto done_locked;
		}
		memcpy(q->instances, p->instances,
		    p->instances_len * sizeof(int));
	    }
	}
    }

    /*
     * The call to pmNewContext (above) should have connected to the pmcd.
     * Make sure the new profile will be sent before the next fetch.
     */
    newcon->c_sent = 0;

    /* clone the archive control struct, if any */
    if (newcon->c_archctl != NULL)
	__pmArchCtlFree(newcon->c_archctl); /* will allocate a new one below */
    if (oldcon->c_archctl != NULL) {
	if ((newcon->c_archctl = (__pmArchCtl *)malloc(sizeof(__pmArchCtl))) == NULL) {
	    sts = -oserror();
	    goto done_locked;
	}
	*newcon->c_archctl = *oldcon->c_archctl;	/* struct assignment */
	/*
	 * Need to make hash list and read cache independent in case oldcon
	 * is subsequently closed via pmDestroyContext() and don't want
	 * __pmFreeInterpData() to trash our hash list and read cache.
	 * Start with an empty hash list and read cache for the dup'd context.
	 */
	newcon->c_archctl->ac_pmid_hc.nodes = 0;
	newcon->c_archctl->ac_pmid_hc.hsize = 0;
	newcon->c_archctl->ac_cache = NULL;

	/*
	 * We need to copy the log lists and bump up the reference counts of
	 * any open logs.
	 */
	if (oldcon->c_archctl->ac_log_list != NULL) {
	    size_t size = oldcon->c_archctl->ac_num_logs *
		sizeof(*oldcon->c_archctl->ac_log_list);
	    if ((newcon->c_archctl->ac_log_list = malloc(size)) == NULL) {
		sts = -oserror();
		free(newcon->c_archctl);
		goto done_locked;
	    }
	    /* We need to duplicate each ac_log_list entry. */
	    for (i = 0; i < newcon->c_archctl->ac_num_logs; i++) {
		newcon->c_archctl->ac_log_list[i] =
		    malloc(sizeof(*newcon->c_archctl->ac_log_list[i]));
		newmlcp = newcon->c_archctl->ac_log_list[i];
		oldmlcp = oldcon->c_archctl->ac_log_list[i];
		*newmlcp = *oldmlcp;
		/*
		 * We need to duplicate the ml_name and the ml_hostname of each
		 * archive in the list.
		 */
		if ((newmlcp->ml_name = strdup (newmlcp->ml_name)) == NULL) {
		    sts = -oserror();
		    goto done_locked;
		}
		if ((newmlcp->ml_hostname = strdup (newmlcp->ml_hostname)) == NULL) {
		    sts = -oserror();
		    goto done_locked;
		}
		if ((newmlcp->ml_tz = strdup (newmlcp->ml_tz)) == NULL) {
		    sts = -oserror();
		    goto done_locked;
		}
	    }
	    /* We need to bump up the reference count of the ac_log. */
	    if (newcon->c_archctl->ac_log != NULL)
		++newcon->c_archctl->ac_log->l_refcnt;
	}
    }

    sts = new;

done_locked:
    PM_UNLOCK(oldcon->c_lock);
    PM_UNLOCK(newcon->c_lock);

done:
    /* return an error code, or the handle for the new context */
    if (sts < 0 && new >= 0)
	contexts_map[ctxnum] = MAP_FREE;
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT) {
	fprintf(stderr, "pmDupContext() -> %d\n", sts);
	if (sts >= 0)
	    __pmDumpContext(stderr, sts, PM_INDOM_NULL);
    }
#endif

    return sts;
}

int
pmUseContext(int handle)
{
    int		ctxnum;

    PM_INIT_LOCKS();

    PM_LOCK(contexts_lock);
    if ((ctxnum = map_handle(handle)) < 0) {
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT)
	    fprintf(stderr, "pmUseContext(%d) -> %d\n", handle, PM_ERR_NOCONTEXT);
#endif
	PM_UNLOCK(contexts_lock);
	return PM_ERR_NOCONTEXT;
    }

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmUseContext(%d) -> contexts[%d]\n", handle, ctxnum);
#endif
    PM_TPD(curr_handle) = handle;
    PM_TPD(curr_ctxp) = contexts[ctxnum];

    PM_UNLOCK(contexts_lock);
    return 0;
}

static void
__pmPMCDCtlFree(__pmPMCDCtl *cp)
{
    struct linger	dolinger = {0, 1};

    PM_LOCK(cp->pc_lock);
    if (--cp->pc_refcnt != 0) {
	PM_UNLOCK(cp->pc_lock);
	return;
    }
    if (cp->pc_fd >= 0) {
	/* before close, unsent data should be flushed */
	__pmSetSockOpt(cp->pc_fd, SOL_SOCKET, SO_LINGER,
			(char *)&dolinger, (__pmSockLen)sizeof(dolinger));
	__pmCloseSocket(cp->pc_fd);
    }
    __pmFreeHostSpec(cp->pc_hosts, cp->pc_nhosts);
    PM_UNLOCK(cp->pc_lock);
#if 0
    /* TODO ... not sure what's right here ... destroy the lock before
     * all use has necessarily gone away or leak memory?
     */
    destroylock(&cp->pc_lock, "pc_lock");
#endif
    free(cp);
}

int
pmDestroyContext(int handle)
{
    __pmContext	*ctxp;
    int		ctxnum;

    PM_INIT_LOCKS();

    PM_LOCK(contexts_lock);
    if ((ctxnum = map_handle(handle)) < 0) {
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmDestroyContext(%d) -> %d\n", handle, PM_ERR_NOCONTEXT);
#endif
	PM_UNLOCK(contexts_lock);
	return PM_ERR_NOCONTEXT;
    }

    ctxp = contexts[ctxnum];
    PM_LOCK(ctxp->c_lock);
    contexts_map[ctxnum] = MAP_TEARDOWN;
    PM_UNLOCK(contexts_lock);
    if (ctxp->c_pmcd != NULL) {
	__pmPMCDCtlFree(ctxp->c_pmcd);
	ctxp->c_pmcd = NULL;
    }
    if (ctxp->c_archctl != NULL) {
	__pmFreeInterpData(ctxp);
	__pmArchCtlFree(ctxp->c_archctl);
	ctxp->c_archctl = NULL;
    }
    __pmFreeAttrsSpec(&ctxp->c_attrs);
    /* Note: __pmHashClear sets c_attrs.hsize = 0 and c_attrs.hash = NULL */
    __pmHashClear(&ctxp->c_attrs);

    if (handle == PM_TPD(curr_handle)) {
	/* we have no choice */
	PM_TPD(curr_handle) = PM_CONTEXT_UNDEF;
	PM_TPD(curr_ctxp) = NULL;
    }

    __pmFreeProfile(ctxp->c_instprof);
    ctxp->c_instprof = NULL;
    /* Note: __dmclosecontext sets ctxp->c_dm = NULL */
    __dmclosecontext(ctxp);
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT)
	fprintf(stderr, "pmDestroyContext(%d) -> 0, curr_handle=%d\n",
		handle, PM_TPD(curr_handle));
#endif

    PM_UNLOCK(ctxp->c_lock);

    PM_LOCK(contexts_lock);
    contexts_map[ctxnum] = MAP_FREE;
    PM_UNLOCK(contexts_lock);

    return 0;
}

static const char *_mode[] = { "LIVE", "INTERP", "FORW", "BACK" };

/*
 * dump context(s); context == -1 for all contexts, indom == PM_INDOM_NULL
 * for all instance domains.
 *
 * Threadsafe Note:
 *	contexts_lock mutex is not acquired here ... need to avoid
 *	nested locking, and this is only a diagnostic routine so
 *	any data race is an acceptable trade-off
 */
void
__pmDumpContext(FILE *f, int context, pmInDom indom)
{
    int			i, j;
    __pmContext		*con;

    PM_INIT_LOCKS();

    fprintf(f, "Dump Contexts: current -> contexts[%d] handle %d\n",
	map_handle_nolock(PM_TPD(curr_handle)), PM_TPD(curr_handle));
    if (PM_TPD(curr_handle) < 0)
	return;

    if (indom != PM_INDOM_NULL) {
	char	strbuf[20];
	fprintf(f, "Dump restricted to indom=%d [%s]\n", 
	        indom, pmInDomStr_r(indom, strbuf, sizeof(strbuf)));
    }

    PM_LOCK(contexts_lock);
    for (i = 0; i < contexts_len; i++) {
	con = contexts[i];
	if (context == -1 || context == i) {
	    fprintf(f, "contexts[%d]", i);
            if (contexts_map[i] == MAP_FREE) {
		fprintf(f, " free\n");
                continue;
            }
            else if (contexts_map[i] == MAP_TEARDOWN) {
		fprintf(f, " being destroyed\n");
                continue;
            }
            else if (con->c_type == PM_CONTEXT_INIT) {
		fprintf(f, " init\n");
                continue;
            }
	    else if (con->c_type == PM_CONTEXT_HOST) {
		fprintf(f, " handle %d:", contexts_map[i]);
		PM_LOCK(con->c_pmcd->pc_lock);
		fprintf(f, " host %s:", con->c_pmcd->pc_hosts[0].name);
		fprintf(f, " pmcd=%s profile=%s fd=%d refcnt=%d",
		    (con->c_pmcd->pc_fd < 0) ? "NOT CONNECTED" : "CONNECTED",
		    con->c_sent ? "SENT" : "NOT_SENT",
		    con->c_pmcd->pc_fd,
		    con->c_pmcd->pc_refcnt);
		if (con->c_flags)
		    fprintf(f, " flags=%x", con->c_flags);
		PM_UNLOCK(con->c_pmcd->pc_lock);
	    }
	    else if (con->c_type == PM_CONTEXT_LOCAL) {
		fprintf(f, " handle %d:", contexts_map[i]);
		fprintf(f, " standalone:");
		fprintf(f, " profile=%s\n",
		    con->c_sent ? "SENT" : "NOT_SENT");
	    }
	    else if (con->c_type == PM_CONTEXT_ARCHIVE) {
		fprintf(f, " handle %d:", contexts_map[i]);
		for (j = 0; j < con->c_archctl->ac_num_logs; j++) {
		    fprintf(f, " log %s:",
			    con->c_archctl->ac_log_list[j]->ml_name);
		    if (con->c_archctl->ac_log == NULL ||
			con->c_archctl->ac_log->l_refcnt == 0 ||
			strcmp (con->c_archctl->ac_log_list[j]->ml_name,
				con->c_archctl->ac_log->l_name) != 0) {
			fprintf(f, " not open\n");
			continue;
		    }
		    fprintf(f, " mode=%s", _mode[con->c_mode & __PM_MODE_MASK]);
		    fprintf(f, " profile=%s tifd=%d mdfd=%d mfd=%d\nrefcnt=%d vol=%d",
			    con->c_sent ? "SENT" : "NOT_SENT",
			    con->c_archctl->ac_log->l_tifp == NULL ? -1 : fileno(con->c_archctl->ac_log->l_tifp),
			    fileno(con->c_archctl->ac_log->l_mdfp),
			    fileno(con->c_archctl->ac_log->l_mfp),
			    con->c_archctl->ac_log->l_refcnt,
			    con->c_archctl->ac_log->l_curvol);
		    fprintf(f, " offset=%ld (vol=%d) serial=%d",
			    (long)con->c_archctl->ac_offset,
			    con->c_archctl->ac_vol,
			    con->c_archctl->ac_serial);
		}
	    }
	    if (con->c_type == PM_CONTEXT_HOST || con->c_type == PM_CONTEXT_ARCHIVE) {
		fprintf(f, " origin=%d.%06d",
		    con->c_origin.tv_sec, con->c_origin.tv_usec);
		fprintf(f, " delta=%d\n", con->c_delta);
	    }
	    __pmDumpProfile(f, indom, con->c_instprof);
	}
    }

    PM_UNLOCK(contexts_lock);
}

#define TYPESTRLEN 20
/*
 * Come here after TIMEOUT or IPC error at the PDU layer in the
 * communication with another process (usually during a __pmGetPDU()
 * call.
 *
 * If we are a client of pmcd, then close the channel to prevent
 * cascading errors from any context that is communicating with
 * the same pmcd over the same socket.
 *
 * If we are not a client of pmcd (e.g. pmcd or dbpmda commnicating
 * with a PMDA), do nothing ... the higher level logic will handle
 * the error and there is no socket multiplexing in play.
 *
 * We were expecting a PDU of type expect, but received one
 * of type recv (which may be an error, e.g PM_ERR_TIMEOUT).
 *
 * No need to be delicate here, rip the socket down so other
 * contexts are not compromised by stale data on the channel.
 *
 * If the channel's fd is < 0, we've been here before (or someone
 * else has nuked the channel), so be silent.
 */
void
__pmCloseChannelbyFd(int fd, int expect, int recv)
{
    char	errmsg[PM_MAXERRMSGLEN];
    char	expect_str[TYPESTRLEN];
    char	recv_str[TYPESTRLEN];
    if (__pmGetInternalState() == PM_STATE_PMCS) {
	/* not a client of pmcd, so don't close any channels here */
	return;
    }
    __pmPDUTypeStr_r(expect, expect_str, TYPESTRLEN);
    if (recv < 0) {
	/* error or timeout */
	__pmNotifyErr(LOG_ERR, "__pmCloseChannelbyFd: fd=%d expected PDU_%s received: %s",
	    fd, expect_str, pmErrStr_r(recv, errmsg, sizeof(errmsg)));
    }
    else if (recv > 0) {
	/* wrong pdu type */
	__pmPDUTypeStr_r(recv, recv_str, TYPESTRLEN);
	__pmNotifyErr(LOG_ERR, "__pmCloseChannelbyFd: fd=%d expected PDU_%s received: PDU_%s",
	    fd, expect_str, recv_str);
    }
    else {
	/* EOF aka PDU-0, nothing to report */
	;
    }
    __pmCloseSocket(fd);
}

/*
 * See comments above for __pmCloseChannelbyFd() ...
 *
 * In this case we are dealing with a PMAPI context and come here
 * holding the c_lock.
 */
void
__pmCloseChannelbyContext(__pmContext *ctxp, int expect, int recv)
{
    if (__pmGetInternalState() == PM_STATE_PMCS) {
	/* not a client of pmcd, so don't close any channels here */
	return;
    }
    PM_UNLOCK(ctxp->c_pmcd->pc_lock);
    /* guard against repeated calls for the same channel ... */
    if (ctxp->c_pmcd->pc_fd >= 0) {
	char	errmsg[PM_MAXERRMSGLEN];
	char	expect_str[TYPESTRLEN];
	char	recv_str[TYPESTRLEN];
	__pmPDUTypeStr_r(expect, expect_str, TYPESTRLEN);
	if (recv < 0) {
	    /* error or timeout */
	    __pmNotifyErr(LOG_ERR, "__pmCloseChannelbyContext: fd=%d context=%d expected PDU_%s received: %s",
		ctxp->c_pmcd->pc_fd, __pmPtrToHandle(ctxp), expect_str, pmErrStr_r(recv, errmsg, sizeof(errmsg)));
	}
	else if (recv > 0) {
	    /* wrong pdu type */
	    __pmPDUTypeStr_r(recv, recv_str, TYPESTRLEN);
	    __pmNotifyErr(LOG_ERR, "__pmCloseChannelbyContext: fd=%d context=%d expected PDU_%s received: PDU_%s",
		ctxp->c_pmcd->pc_fd, __pmPtrToHandle(ctxp), expect_str, recv_str);
	}
	else {
	    /* EOF aka PDU-0, nothing to report */
	    ;
	}
	__pmCloseSocket(ctxp->c_pmcd->pc_fd);
	ctxp->c_pmcd->pc_fd = -1;
    }
    PM_UNLOCK(ctxp->c_pmcd->pc_lock);
}

#ifdef PM_MULTI_THREAD
#ifdef PM_MULTI_THREAD_DEBUG
/*
 * return context if lock == c_lock for a context ... no locking here
 * to avoid recursion ad nauseum
 */
int
__pmIsContextLock(void *lock)
{
    int		i;
    for (i = 0; i < contexts_len; i++) {
	if ((void *)&contexts[i]->c_lock == lock)
	    return i;
    }
    return -1;
}

/*
 * return context if lock == pc_lock for a context ... no locking here
 * to avoid recursion ad nauseum
 */
int
__pmIsChannelLock(void *lock)
{
    int		i;
    for (i = 0; i < contexts_len; i++) {
	if ((void *)&contexts[i]->c_pmcd->pc_lock == lock)
	    return i;
    }
    return -1;
}
#endif
#endif
