// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * Fault Management Architecture (FMA) Resource and Protocol Support
 *
 * The routines contained herein provide services to support kernel subsystems
 * in publishing fault management telemetry (see PSARC 2002/412 and 2003/089).
 *
 * Name-Value Pair Lists
 *
 * The embodiment of an FMA protocol element (event, fmri or authority) is a
 * name-value pair list (nvlist_t).  FMA-specific nvlist constructor and
 * destructor functions, fm_nvlist_create() and fm_nvlist_destroy(), are used
 * to create an nvpair list using custom allocators.  Callers may choose to
 * allocate either from the kernel memory allocator, or from a preallocated
 * buffer, useful in constrained contexts like high-level interrupt routines.
 *
 * Protocol Event and FMRI Construction
 *
 * Convenience routines are provided to construct nvlist events according to
 * the FMA Event Protocol and Naming Schema specification for ereports and
 * FMRIs for the dev, cpu, hc, mem, legacy hc and de schemes.
 *
 * ENA Manipulation
 *
 * Routines to generate ENA formats 0, 1 and 2 are available as well as
 * routines to increment formats 1 and 2.  Individual fields within the
 * ENA are extractable via fm_ena_time_get(), fm_ena_id_get(),
 * fm_ena_format_get() and fm_ena_gen_get().
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/list.h>
#include <sys/nvpair.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/sunddi.h>
#include <sys/systeminfo.h>
#include <sys/fm/util.h>
#include <sys/fm/protocol.h>
#include <sys/kstat.h>
#include <sys/zfs_context.h>
#ifdef _KERNEL
#include <sys/atomic.h>
#include <sys/condvar.h>
#include <sys/zfs_ioctl.h>

static uint_t zfs_zevent_len_max = 512;

static uint_t zevent_len_cur = 0;
static int zevent_waiters = 0;
static int zevent_flags = 0;

/* Num events rate limited since the last time zfs_zevent_next() was called */
static uint64_t ratelimit_dropped = 0;

/*
 * The EID (Event IDentifier) is used to uniquely tag a zevent when it is
 * posted.  The posted EIDs are monotonically increasing but not persistent.
 * They will be reset to the initial value (1) each time the kernel module is
 * loaded.
 */
static uint64_t zevent_eid = 0;

static kmutex_t zevent_lock;
static list_t zevent_list;
static kcondvar_t zevent_cv;
#endif /* _KERNEL */


/*
 * Common fault management kstats to record event generation failures
 */

struct erpt_kstat {
	kstat_named_t	erpt_dropped;		/* num erpts dropped on post */
	kstat_named_t	erpt_set_failed;	/* num erpt set failures */
	kstat_named_t	fmri_set_failed;	/* num fmri set failures */
	kstat_named_t	payload_set_failed;	/* num payload set failures */
	kstat_named_t	erpt_duplicates;	/* num duplicate erpts */
};

static struct erpt_kstat erpt_kstat_data = {
	{ "erpt-dropped", KSTAT_DATA_UINT64 },
	{ "erpt-set-failed", KSTAT_DATA_UINT64 },
	{ "fmri-set-failed", KSTAT_DATA_UINT64 },
	{ "payload-set-failed", KSTAT_DATA_UINT64 },
	{ "erpt-duplicates", KSTAT_DATA_UINT64 }
};

kstat_t *fm_ksp;

#ifdef _KERNEL

static zevent_t *
zfs_zevent_alloc(void)
{
	zevent_t *ev;

	ev = kmem_zalloc(sizeof (zevent_t), KM_SLEEP);

	list_create(&ev->ev_ze_list, sizeof (zfs_zevent_t),
	    offsetof(zfs_zevent_t, ze_node));
	list_link_init(&ev->ev_node);

	return (ev);
}

static void
zfs_zevent_free(zevent_t *ev)
{
	/* Run provided cleanup callback */
	ev->ev_cb(ev->ev_nvl, ev->ev_detector);

	list_destroy(&ev->ev_ze_list);
	kmem_free(ev, sizeof (zevent_t));
}

static void
zfs_zevent_drain(zevent_t *ev)
{
	zfs_zevent_t *ze;

	ASSERT(MUTEX_HELD(&zevent_lock));
	list_remove(&zevent_list, ev);

	/* Remove references to this event in all private file data */
	while ((ze = list_remove_head(&ev->ev_ze_list)) != NULL) {
		ze->ze_zevent = NULL;
		ze->ze_dropped++;
	}

	zfs_zevent_free(ev);
}

void
zfs_zevent_drain_all(uint_t *count)
{
	zevent_t *ev;

	mutex_enter(&zevent_lock);
	while ((ev = list_head(&zevent_list)) != NULL)
		zfs_zevent_drain(ev);

	*count = zevent_len_cur;
	zevent_len_cur = 0;
	mutex_exit(&zevent_lock);
}

/*
 * New zevents are inserted at the head.  If the maximum queue
 * length is exceeded a zevent will be drained from the tail.
 * As part of this any user space processes which currently have
 * a reference to this zevent_t in their private data will have
 * this reference set to NULL.
 */
static void
zfs_zevent_insert(zevent_t *ev)
{
	ASSERT(MUTEX_HELD(&zevent_lock));
	list_insert_head(&zevent_list, ev);

	if (zevent_len_cur >= zfs_zevent_len_max)
		zfs_zevent_drain(list_tail(&zevent_list));
	else
		zevent_len_cur++;
}

/*
 * Post a zevent. The cb will be called when nvl and detector are no longer
 * needed, i.e.:
 * - An error happened and a zevent can't be posted. In this case, cb is called
 *   before zfs_zevent_post() returns.
 * - The event is being drained and freed.
 */
int
zfs_zevent_post(nvlist_t *nvl, nvlist_t *detector, zevent_cb_t *cb)
{
	inode_timespec_t tv;
	int64_t tv_array[2];
	uint64_t eid;
	size_t nvl_size = 0;
	zevent_t *ev;
	int error;

	ASSERT(cb != NULL);

	gethrestime(&tv);
	tv_array[0] = tv.tv_sec;
	tv_array[1] = tv.tv_nsec;

	error = nvlist_add_int64_array(nvl, FM_EREPORT_TIME, tv_array, 2);
	if (error) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
		goto out;
	}

	eid = atomic_inc_64_nv(&zevent_eid);
	error = nvlist_add_uint64(nvl, FM_EREPORT_EID, eid);
	if (error) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
		goto out;
	}

	error = nvlist_size(nvl, &nvl_size, NV_ENCODE_NATIVE);
	if (error) {
		atomic_inc_64(&erpt_kstat_data.erpt_dropped.value.ui64);
		goto out;
	}

	if (nvl_size > ERPT_DATA_SZ || nvl_size == 0) {
		atomic_inc_64(&erpt_kstat_data.erpt_dropped.value.ui64);
		error = EOVERFLOW;
		goto out;
	}

	ev = zfs_zevent_alloc();
	if (ev == NULL) {
		atomic_inc_64(&erpt_kstat_data.erpt_dropped.value.ui64);
		error = ENOMEM;
		goto out;
	}

	ev->ev_nvl = nvl;
	ev->ev_detector = detector;
	ev->ev_cb = cb;
	ev->ev_eid = eid;

	mutex_enter(&zevent_lock);
	zfs_zevent_insert(ev);
	cv_broadcast(&zevent_cv);
	mutex_exit(&zevent_lock);

out:
	if (error)
		cb(nvl, detector);

	return (error);
}

void
zfs_zevent_track_duplicate(void)
{
	atomic_inc_64(&erpt_kstat_data.erpt_duplicates.value.ui64);
}

static int
zfs_zevent_minor_to_state(minor_t minor, zfs_zevent_t **ze)
{
	*ze = zfsdev_get_state(minor, ZST_ZEVENT);
	if (*ze == NULL)
		return (SET_ERROR(EBADF));

	return (0);
}

zfs_file_t *
zfs_zevent_fd_hold(int fd, minor_t *minorp, zfs_zevent_t **ze)
{
	zfs_file_t *fp = zfs_file_get(fd);
	if (fp == NULL)
		return (NULL);

	int error = zfsdev_getminor(fp, minorp);
	if (error == 0)
		error = zfs_zevent_minor_to_state(*minorp, ze);

	if (error) {
		zfs_zevent_fd_rele(fp);
		fp = NULL;
	}

	return (fp);
}

void
zfs_zevent_fd_rele(zfs_file_t *fp)
{
	zfs_file_put(fp);
}

/*
 * Get the next zevent in the stream and place a copy in 'event'.  This
 * may fail with ENOMEM if the encoded nvlist size exceeds the passed
 * 'event_size'.  In this case the stream pointer is not advanced and
 * and 'event_size' is set to the minimum required buffer size.
 */
int
zfs_zevent_next(zfs_zevent_t *ze, nvlist_t **event, uint64_t *event_size,
    uint64_t *dropped)
{
	zevent_t *ev;
	size_t size;
	int error = 0;

	mutex_enter(&zevent_lock);
	if (ze->ze_zevent == NULL) {
		/* New stream start at the beginning/tail */
		ev = list_tail(&zevent_list);
		if (ev == NULL) {
			error = ENOENT;
			goto out;
		}
	} else {
		/*
		 * Existing stream continue with the next element and remove
		 * ourselves from the wait queue for the previous element
		 */
		ev = list_prev(&zevent_list, ze->ze_zevent);
		if (ev == NULL) {
			error = ENOENT;
			goto out;
		}
	}

	VERIFY(nvlist_size(ev->ev_nvl, &size, NV_ENCODE_NATIVE) == 0);
	if (size > *event_size) {
		*event_size = size;
		error = ENOMEM;
		goto out;
	}

	if (ze->ze_zevent)
		list_remove(&ze->ze_zevent->ev_ze_list, ze);

	ze->ze_zevent = ev;
	list_insert_head(&ev->ev_ze_list, ze);
	(void) nvlist_dup(ev->ev_nvl, event, KM_SLEEP);
	*dropped = ze->ze_dropped;

#ifdef _KERNEL
	/* Include events dropped due to rate limiting */
	*dropped += atomic_swap_64(&ratelimit_dropped, 0);
#endif
	ze->ze_dropped = 0;
out:
	mutex_exit(&zevent_lock);

	return (error);
}

/*
 * Wait in an interruptible state for any new events.
 */
int
zfs_zevent_wait(zfs_zevent_t *ze)
{
	int error = EAGAIN;

	mutex_enter(&zevent_lock);
	zevent_waiters++;

	while (error == EAGAIN) {
		if (zevent_flags & ZEVENT_SHUTDOWN) {
			error = SET_ERROR(ESHUTDOWN);
			break;
		}

		if (cv_wait_sig(&zevent_cv, &zevent_lock) == 0) {
			error = SET_ERROR(EINTR);
			break;
		} else if (!list_is_empty(&zevent_list)) {
			error = 0;
			continue;
		} else {
			error = EAGAIN;
		}
	}

	zevent_waiters--;
	mutex_exit(&zevent_lock);

	return (error);
}

/*
 * The caller may seek to a specific EID by passing that EID.  If the EID
 * is still available in the posted list of events the cursor is positioned
 * there.  Otherwise ENOENT is returned and the cursor is not moved.
 *
 * There are two reserved EIDs which may be passed and will never fail.
 * ZEVENT_SEEK_START positions the cursor at the start of the list, and
 * ZEVENT_SEEK_END positions the cursor at the end of the list.
 */
int
zfs_zevent_seek(zfs_zevent_t *ze, uint64_t eid)
{
	zevent_t *ev;
	int error = 0;

	mutex_enter(&zevent_lock);

	if (eid == ZEVENT_SEEK_START) {
		if (ze->ze_zevent)
			list_remove(&ze->ze_zevent->ev_ze_list, ze);

		ze->ze_zevent = NULL;
		goto out;
	}

	if (eid == ZEVENT_SEEK_END) {
		if (ze->ze_zevent)
			list_remove(&ze->ze_zevent->ev_ze_list, ze);

		ev = list_head(&zevent_list);
		if (ev) {
			ze->ze_zevent = ev;
			list_insert_head(&ev->ev_ze_list, ze);
		} else {
			ze->ze_zevent = NULL;
		}

		goto out;
	}

	for (ev = list_tail(&zevent_list); ev != NULL;
	    ev = list_prev(&zevent_list, ev)) {
		if (ev->ev_eid == eid) {
			if (ze->ze_zevent)
				list_remove(&ze->ze_zevent->ev_ze_list, ze);

			ze->ze_zevent = ev;
			list_insert_head(&ev->ev_ze_list, ze);
			break;
		}
	}

	if (ev == NULL)
		error = ENOENT;

out:
	mutex_exit(&zevent_lock);

	return (error);
}

void
zfs_zevent_init(zfs_zevent_t **zep)
{
	zfs_zevent_t *ze;

	ze = *zep = kmem_zalloc(sizeof (zfs_zevent_t), KM_SLEEP);
	list_link_init(&ze->ze_node);
}

void
zfs_zevent_destroy(zfs_zevent_t *ze)
{
	mutex_enter(&zevent_lock);
	if (ze->ze_zevent)
		list_remove(&ze->ze_zevent->ev_ze_list, ze);
	mutex_exit(&zevent_lock);

	kmem_free(ze, sizeof (zfs_zevent_t));
}
#endif /* _KERNEL */

/*
 * Wrappers for FM nvlist allocators
 */
static void *
i_fm_alloc(nv_alloc_t *nva, size_t size)
{
	(void) nva;
	return (kmem_alloc(size, KM_SLEEP));
}

static void
i_fm_free(nv_alloc_t *nva, void *buf, size_t size)
{
	(void) nva;
	kmem_free(buf, size);
}

static const nv_alloc_ops_t fm_mem_alloc_ops = {
	.nv_ao_init = NULL,
	.nv_ao_fini = NULL,
	.nv_ao_alloc = i_fm_alloc,
	.nv_ao_free = i_fm_free,
	.nv_ao_reset = NULL
};

/*
 * Create and initialize a new nv_alloc_t for a fixed buffer, buf.  A pointer
 * to the newly allocated nv_alloc_t structure is returned upon success or NULL
 * is returned to indicate that the nv_alloc structure could not be created.
 */
nv_alloc_t *
fm_nva_xcreate(char *buf, size_t bufsz)
{
	nv_alloc_t *nvhdl = kmem_zalloc(sizeof (nv_alloc_t), KM_SLEEP);

	if (bufsz == 0 || nv_alloc_init(nvhdl, nv_fixed_ops, buf, bufsz) != 0) {
		kmem_free(nvhdl, sizeof (nv_alloc_t));
		return (NULL);
	}

	return (nvhdl);
}

/*
 * Destroy a previously allocated nv_alloc structure.  The fixed buffer
 * associated with nva must be freed by the caller.
 */
void
fm_nva_xdestroy(nv_alloc_t *nva)
{
	nv_alloc_fini(nva);
	kmem_free(nva, sizeof (nv_alloc_t));
}

/*
 * Create a new nv list.  A pointer to a new nv list structure is returned
 * upon success or NULL is returned to indicate that the structure could
 * not be created.  The newly created nv list is created and managed by the
 * operations installed in nva.   If nva is NULL, the default FMA nva
 * operations are installed and used.
 *
 * When called from the kernel and nva == NULL, this function must be called
 * from passive kernel context with no locks held that can prevent a
 * sleeping memory allocation from occurring.  Otherwise, this function may
 * be called from other kernel contexts as long a valid nva created via
 * fm_nva_create() is supplied.
 */
nvlist_t *
fm_nvlist_create(nv_alloc_t *nva)
{
	int hdl_alloced = 0;
	nvlist_t *nvl;
	nv_alloc_t *nvhdl;

	if (nva == NULL) {
		nvhdl = kmem_zalloc(sizeof (nv_alloc_t), KM_SLEEP);

		if (nv_alloc_init(nvhdl, &fm_mem_alloc_ops, NULL, 0) != 0) {
			kmem_free(nvhdl, sizeof (nv_alloc_t));
			return (NULL);
		}
		hdl_alloced = 1;
	} else {
		nvhdl = nva;
	}

	if (nvlist_xalloc(&nvl, NV_UNIQUE_NAME, nvhdl) != 0) {
		if (hdl_alloced) {
			nv_alloc_fini(nvhdl);
			kmem_free(nvhdl, sizeof (nv_alloc_t));
		}
		return (NULL);
	}

	return (nvl);
}

/*
 * Destroy a previously allocated nvlist structure.  flag indicates whether
 * or not the associated nva structure should be freed (FM_NVA_FREE) or
 * retained (FM_NVA_RETAIN).  Retaining the nv alloc structure allows
 * it to be re-used for future nvlist creation operations.
 */
void
fm_nvlist_destroy(nvlist_t *nvl, int flag)
{
	nv_alloc_t *nva = nvlist_lookup_nv_alloc(nvl);

	nvlist_free(nvl);

	if (nva != NULL) {
		if (flag == FM_NVA_FREE)
			fm_nva_xdestroy(nva);
	}
}

int
i_fm_payload_set(nvlist_t *payload, const char *name, va_list ap)
{
	int nelem, ret = 0;
	data_type_t type;

	while (ret == 0 && name != NULL) {
		type = va_arg(ap, data_type_t);
		switch (type) {
		case DATA_TYPE_BYTE:
			ret = nvlist_add_byte(payload, name,
			    va_arg(ap, uint_t));
			break;
		case DATA_TYPE_BYTE_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_byte_array(payload, name,
			    va_arg(ap, uchar_t *), nelem);
			break;
		case DATA_TYPE_BOOLEAN_VALUE:
			ret = nvlist_add_boolean_value(payload, name,
			    va_arg(ap, boolean_t));
			break;
		case DATA_TYPE_BOOLEAN_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_boolean_array(payload, name,
			    va_arg(ap, boolean_t *), nelem);
			break;
		case DATA_TYPE_INT8:
			ret = nvlist_add_int8(payload, name,
			    va_arg(ap, int));
			break;
		case DATA_TYPE_INT8_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_int8_array(payload, name,
			    va_arg(ap, int8_t *), nelem);
			break;
		case DATA_TYPE_UINT8:
			ret = nvlist_add_uint8(payload, name,
			    va_arg(ap, uint_t));
			break;
		case DATA_TYPE_UINT8_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_uint8_array(payload, name,
			    va_arg(ap, uint8_t *), nelem);
			break;
		case DATA_TYPE_INT16:
			ret = nvlist_add_int16(payload, name,
			    va_arg(ap, int));
			break;
		case DATA_TYPE_INT16_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_int16_array(payload, name,
			    va_arg(ap, int16_t *), nelem);
			break;
		case DATA_TYPE_UINT16:
			ret = nvlist_add_uint16(payload, name,
			    va_arg(ap, uint_t));
			break;
		case DATA_TYPE_UINT16_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_uint16_array(payload, name,
			    va_arg(ap, uint16_t *), nelem);
			break;
		case DATA_TYPE_INT32:
			ret = nvlist_add_int32(payload, name,
			    va_arg(ap, int32_t));
			break;
		case DATA_TYPE_INT32_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_int32_array(payload, name,
			    va_arg(ap, int32_t *), nelem);
			break;
		case DATA_TYPE_UINT32:
			ret = nvlist_add_uint32(payload, name,
			    va_arg(ap, uint32_t));
			break;
		case DATA_TYPE_UINT32_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_uint32_array(payload, name,
			    va_arg(ap, uint32_t *), nelem);
			break;
		case DATA_TYPE_INT64:
			ret = nvlist_add_int64(payload, name,
			    va_arg(ap, int64_t));
			break;
		case DATA_TYPE_INT64_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_int64_array(payload, name,
			    va_arg(ap, int64_t *), nelem);
			break;
		case DATA_TYPE_UINT64:
			ret = nvlist_add_uint64(payload, name,
			    va_arg(ap, uint64_t));
			break;
		case DATA_TYPE_UINT64_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_uint64_array(payload, name,
			    va_arg(ap, uint64_t *), nelem);
			break;
		case DATA_TYPE_STRING:
			ret = nvlist_add_string(payload, name,
			    va_arg(ap, char *));
			break;
		case DATA_TYPE_STRING_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_string_array(payload, name,
			    va_arg(ap, const char **), nelem);
			break;
		case DATA_TYPE_NVLIST:
			ret = nvlist_add_nvlist(payload, name,
			    va_arg(ap, nvlist_t *));
			break;
		case DATA_TYPE_NVLIST_ARRAY:
			nelem = va_arg(ap, int);
			ret = nvlist_add_nvlist_array(payload, name,
			    va_arg(ap, const nvlist_t **), nelem);
			break;
		default:
			ret = EINVAL;
		}

		name = va_arg(ap, char *);
	}
	return (ret);
}

void
fm_payload_set(nvlist_t *payload, ...)
{
	int ret;
	const char *name;
	va_list ap;

	va_start(ap, payload);
	name = va_arg(ap, char *);
	ret = i_fm_payload_set(payload, name, ap);
	va_end(ap);

	if (ret)
		atomic_inc_64(&erpt_kstat_data.payload_set_failed.value.ui64);
}

/*
 * Set-up and validate the members of an ereport event according to:
 *
 *	Member name		Type		Value
 *	====================================================
 *	class			string		ereport
 *	version			uint8_t		0
 *	ena			uint64_t	<ena>
 *	detector		nvlist_t	<detector>
 *	ereport-payload		nvlist_t	<var args>
 *
 * We don't actually add a 'version' member to the payload.  Really,
 * the version quoted to us by our caller is that of the category 1
 * "ereport" event class (and we require FM_EREPORT_VERS0) but
 * the payload version of the actual leaf class event under construction
 * may be something else.  Callers should supply a version in the varargs,
 * or (better) we could take two version arguments - one for the
 * ereport category 1 classification (expect FM_EREPORT_VERS0) and one
 * for the leaf class.
 */
void
fm_ereport_set(nvlist_t *ereport, int version, const char *erpt_class,
    uint64_t ena, const nvlist_t *detector, ...)
{
	char ereport_class[FM_MAX_CLASS];
	const char *name;
	va_list ap;
	int ret;

	if (version != FM_EREPORT_VERS0) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
		return;
	}

	(void) snprintf(ereport_class, FM_MAX_CLASS, "%s.%s",
	    FM_EREPORT_CLASS, erpt_class);
	if (nvlist_add_string(ereport, FM_CLASS, ereport_class) != 0) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_uint64(ereport, FM_EREPORT_ENA, ena)) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
	}

	if (nvlist_add_nvlist(ereport, FM_EREPORT_DETECTOR,
	    (nvlist_t *)detector) != 0) {
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
	}

	va_start(ap, detector);
	name = va_arg(ap, const char *);
	ret = i_fm_payload_set(ereport, name, ap);
	va_end(ap);

	if (ret)
		atomic_inc_64(&erpt_kstat_data.erpt_set_failed.value.ui64);
}

/*
 * Set-up and validate the members of an hc fmri according to;
 *
 *	Member name		Type		Value
 *	===================================================
 *	version			uint8_t		0
 *	auth			nvlist_t	<auth>
 *	hc-name			string		<name>
 *	hc-id			string		<id>
 *
 * Note that auth and hc-id are optional members.
 */

#define	HC_MAXPAIRS	20
#define	HC_MAXNAMELEN	50

static int
fm_fmri_hc_set_common(nvlist_t *fmri, int version, const nvlist_t *auth)
{
	if (version != FM_HC_SCHEME_VERSION) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return (0);
	}

	if (nvlist_add_uint8(fmri, FM_VERSION, version) != 0 ||
	    nvlist_add_string(fmri, FM_FMRI_SCHEME, FM_FMRI_SCHEME_HC) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return (0);
	}

	if (auth != NULL && nvlist_add_nvlist(fmri, FM_FMRI_AUTHORITY,
	    (nvlist_t *)auth) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return (0);
	}

	return (1);
}

void
fm_fmri_hc_set(nvlist_t *fmri, int version, const nvlist_t *auth,
    nvlist_t *snvl, int npairs, ...)
{
	nv_alloc_t *nva = nvlist_lookup_nv_alloc(fmri);
	nvlist_t *pairs[HC_MAXPAIRS];
	va_list ap;
	int i;

	if (!fm_fmri_hc_set_common(fmri, version, auth))
		return;

	npairs = MIN(npairs, HC_MAXPAIRS);

	va_start(ap, npairs);
	for (i = 0; i < npairs; i++) {
		const char *name = va_arg(ap, const char *);
		uint32_t id = va_arg(ap, uint32_t);
		char idstr[11];

		(void) snprintf(idstr, sizeof (idstr), "%u", id);

		pairs[i] = fm_nvlist_create(nva);
		if (nvlist_add_string(pairs[i], FM_FMRI_HC_NAME, name) != 0 ||
		    nvlist_add_string(pairs[i], FM_FMRI_HC_ID, idstr) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
	}
	va_end(ap);

	if (nvlist_add_nvlist_array(fmri, FM_FMRI_HC_LIST,
	    (const nvlist_t **)pairs, npairs) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
	}

	for (i = 0; i < npairs; i++)
		fm_nvlist_destroy(pairs[i], FM_NVA_RETAIN);

	if (snvl != NULL) {
		if (nvlist_add_nvlist(fmri, FM_FMRI_HC_SPECIFIC, snvl) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
	}
}

void
fm_fmri_hc_create(nvlist_t *fmri, int version, const nvlist_t *auth,
    nvlist_t *snvl, nvlist_t *bboard, int npairs, ...)
{
	nv_alloc_t *nva = nvlist_lookup_nv_alloc(fmri);
	nvlist_t *pairs[HC_MAXPAIRS];
	nvlist_t **hcl;
	uint_t n;
	int i, j;
	va_list ap;
	const char *hcname, *hcid;

	if (!fm_fmri_hc_set_common(fmri, version, auth))
		return;

	/*
	 * copy the bboard nvpairs to the pairs array
	 */
	if (nvlist_lookup_nvlist_array(bboard, FM_FMRI_HC_LIST, &hcl, &n)
	    != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	for (i = 0; i < n; i++) {
		if (nvlist_lookup_string(hcl[i], FM_FMRI_HC_NAME,
		    &hcname) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
			return;
		}
		if (nvlist_lookup_string(hcl[i], FM_FMRI_HC_ID, &hcid) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
			return;
		}

		pairs[i] = fm_nvlist_create(nva);
		if (nvlist_add_string(pairs[i], FM_FMRI_HC_NAME, hcname) != 0 ||
		    nvlist_add_string(pairs[i], FM_FMRI_HC_ID, hcid) != 0) {
			for (j = 0; j <= i; j++) {
				if (pairs[j] != NULL)
					fm_nvlist_destroy(pairs[j],
					    FM_NVA_RETAIN);
			}
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
			return;
		}
	}

	/*
	 * create the pairs from passed in pairs
	 */
	npairs = MIN(npairs, HC_MAXPAIRS);

	va_start(ap, npairs);
	for (i = n; i < npairs + n; i++) {
		const char *name = va_arg(ap, const char *);
		uint32_t id = va_arg(ap, uint32_t);
		char idstr[11];
		(void) snprintf(idstr, sizeof (idstr), "%u", id);
		pairs[i] = fm_nvlist_create(nva);
		if (nvlist_add_string(pairs[i], FM_FMRI_HC_NAME, name) != 0 ||
		    nvlist_add_string(pairs[i], FM_FMRI_HC_ID, idstr) != 0) {
			for (j = 0; j <= i; j++) {
				if (pairs[j] != NULL)
					fm_nvlist_destroy(pairs[j],
					    FM_NVA_RETAIN);
			}
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
			va_end(ap);
			return;
		}
	}
	va_end(ap);

	/*
	 * Create the fmri hc list
	 */
	if (nvlist_add_nvlist_array(fmri, FM_FMRI_HC_LIST,
	    (const nvlist_t **)pairs, npairs + n) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	for (i = 0; i < npairs + n; i++) {
			fm_nvlist_destroy(pairs[i], FM_NVA_RETAIN);
	}

	if (snvl != NULL) {
		if (nvlist_add_nvlist(fmri, FM_FMRI_HC_SPECIFIC, snvl) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
			return;
		}
	}
}

/*
 * Set-up and validate the members of an dev fmri according to:
 *
 *	Member name		Type		Value
 *	====================================================
 *	version			uint8_t		0
 *	auth			nvlist_t	<auth>
 *	devpath			string		<devpath>
 *	[devid]			string		<devid>
 *	[target-port-l0id]	string		<target-port-lun0-id>
 *
 * Note that auth and devid are optional members.
 */
void
fm_fmri_dev_set(nvlist_t *fmri_dev, int version, const nvlist_t *auth,
    const char *devpath, const char *devid, const char *tpl0)
{
	int err = 0;

	if (version != DEV_SCHEME_VERSION0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	err |= nvlist_add_uint8(fmri_dev, FM_VERSION, version);
	err |= nvlist_add_string(fmri_dev, FM_FMRI_SCHEME, FM_FMRI_SCHEME_DEV);

	if (auth != NULL) {
		err |= nvlist_add_nvlist(fmri_dev, FM_FMRI_AUTHORITY,
		    (nvlist_t *)auth);
	}

	err |= nvlist_add_string(fmri_dev, FM_FMRI_DEV_PATH, devpath);

	if (devid != NULL)
		err |= nvlist_add_string(fmri_dev, FM_FMRI_DEV_ID, devid);

	if (tpl0 != NULL)
		err |= nvlist_add_string(fmri_dev, FM_FMRI_DEV_TGTPTLUN0, tpl0);

	if (err)
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);

}

/*
 * Set-up and validate the members of an cpu fmri according to:
 *
 *	Member name		Type		Value
 *	====================================================
 *	version			uint8_t		0
 *	auth			nvlist_t	<auth>
 *	cpuid			uint32_t	<cpu_id>
 *	cpumask			uint8_t		<cpu_mask>
 *	serial			uint64_t	<serial_id>
 *
 * Note that auth, cpumask, serial are optional members.
 *
 */
void
fm_fmri_cpu_set(nvlist_t *fmri_cpu, int version, const nvlist_t *auth,
    uint32_t cpu_id, uint8_t *cpu_maskp, const char *serial_idp)
{
	uint64_t *failedp = &erpt_kstat_data.fmri_set_failed.value.ui64;

	if (version < CPU_SCHEME_VERSION1) {
		atomic_inc_64(failedp);
		return;
	}

	if (nvlist_add_uint8(fmri_cpu, FM_VERSION, version) != 0) {
		atomic_inc_64(failedp);
		return;
	}

	if (nvlist_add_string(fmri_cpu, FM_FMRI_SCHEME,
	    FM_FMRI_SCHEME_CPU) != 0) {
		atomic_inc_64(failedp);
		return;
	}

	if (auth != NULL && nvlist_add_nvlist(fmri_cpu, FM_FMRI_AUTHORITY,
	    (nvlist_t *)auth) != 0)
		atomic_inc_64(failedp);

	if (nvlist_add_uint32(fmri_cpu, FM_FMRI_CPU_ID, cpu_id) != 0)
		atomic_inc_64(failedp);

	if (cpu_maskp != NULL && nvlist_add_uint8(fmri_cpu, FM_FMRI_CPU_MASK,
	    *cpu_maskp) != 0)
		atomic_inc_64(failedp);

	if (serial_idp == NULL || nvlist_add_string(fmri_cpu,
	    FM_FMRI_CPU_SERIAL_ID, (char *)serial_idp) != 0)
			atomic_inc_64(failedp);
}

/*
 * Set-up and validate the members of a mem according to:
 *
 *	Member name		Type		Value
 *	====================================================
 *	version			uint8_t		0
 *	auth			nvlist_t	<auth>		[optional]
 *	unum			string		<unum>
 *	serial			string		<serial>	[optional*]
 *	offset			uint64_t	<offset>	[optional]
 *
 *	* serial is required if offset is present
 */
void
fm_fmri_mem_set(nvlist_t *fmri, int version, const nvlist_t *auth,
    const char *unum, const char *serial, uint64_t offset)
{
	if (version != MEM_SCHEME_VERSION0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (!serial && (offset != (uint64_t)-1)) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_uint8(fmri, FM_VERSION, version) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_string(fmri, FM_FMRI_SCHEME, FM_FMRI_SCHEME_MEM) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (auth != NULL) {
		if (nvlist_add_nvlist(fmri, FM_FMRI_AUTHORITY,
		    (nvlist_t *)auth) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
	}

	if (nvlist_add_string(fmri, FM_FMRI_MEM_UNUM, unum) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
	}

	if (serial != NULL) {
		if (nvlist_add_string_array(fmri, FM_FMRI_MEM_SERIAL_ID,
		    (const char **)&serial, 1) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
		if (offset != (uint64_t)-1 && nvlist_add_uint64(fmri,
		    FM_FMRI_MEM_OFFSET, offset) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
	}
}

void
fm_fmri_zfs_set(nvlist_t *fmri, int version, uint64_t pool_guid,
    uint64_t vdev_guid)
{
	if (version != ZFS_SCHEME_VERSION0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_uint8(fmri, FM_VERSION, version) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_string(fmri, FM_FMRI_SCHEME, FM_FMRI_SCHEME_ZFS) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
		return;
	}

	if (nvlist_add_uint64(fmri, FM_FMRI_ZFS_POOL, pool_guid) != 0) {
		atomic_inc_64(&erpt_kstat_data.fmri_set_failed.value.ui64);
	}

	if (vdev_guid != 0) {
		if (nvlist_add_uint64(fmri, FM_FMRI_ZFS_VDEV, vdev_guid) != 0) {
			atomic_inc_64(
			    &erpt_kstat_data.fmri_set_failed.value.ui64);
		}
	}
}

uint64_t
fm_ena_increment(uint64_t ena)
{
	uint64_t new_ena;

	switch (ENA_FORMAT(ena)) {
	case FM_ENA_FMT1:
		new_ena = ena + (1 << ENA_FMT1_GEN_SHFT);
		break;
	case FM_ENA_FMT2:
		new_ena = ena + (1 << ENA_FMT2_GEN_SHFT);
		break;
	default:
		new_ena = 0;
	}

	return (new_ena);
}

uint64_t
fm_ena_generate_cpu(uint64_t timestamp, processorid_t cpuid, uchar_t format)
{
	uint64_t ena = 0;

	switch (format) {
	case FM_ENA_FMT1:
		if (timestamp) {
			ena = (uint64_t)((format & ENA_FORMAT_MASK) |
			    ((cpuid << ENA_FMT1_CPUID_SHFT) &
			    ENA_FMT1_CPUID_MASK) |
			    ((timestamp << ENA_FMT1_TIME_SHFT) &
			    ENA_FMT1_TIME_MASK));
		} else {
			ena = (uint64_t)((format & ENA_FORMAT_MASK) |
			    ((cpuid << ENA_FMT1_CPUID_SHFT) &
			    ENA_FMT1_CPUID_MASK) |
			    ((gethrtime() << ENA_FMT1_TIME_SHFT) &
			    ENA_FMT1_TIME_MASK));
		}
		break;
	case FM_ENA_FMT2:
		ena = (uint64_t)((format & ENA_FORMAT_MASK) |
		    ((timestamp << ENA_FMT2_TIME_SHFT) & ENA_FMT2_TIME_MASK));
		break;
	default:
		break;
	}

	return (ena);
}

uint64_t
fm_ena_generate(uint64_t timestamp, uchar_t format)
{
	uint64_t ena;

	kpreempt_disable();
	ena = fm_ena_generate_cpu(timestamp, getcpuid(), format);
	kpreempt_enable();

	return (ena);
}

uint64_t
fm_ena_generation_get(uint64_t ena)
{
	uint64_t gen;

	switch (ENA_FORMAT(ena)) {
	case FM_ENA_FMT1:
		gen = (ena & ENA_FMT1_GEN_MASK) >> ENA_FMT1_GEN_SHFT;
		break;
	case FM_ENA_FMT2:
		gen = (ena & ENA_FMT2_GEN_MASK) >> ENA_FMT2_GEN_SHFT;
		break;
	default:
		gen = 0;
		break;
	}

	return (gen);
}

uchar_t
fm_ena_format_get(uint64_t ena)
{

	return (ENA_FORMAT(ena));
}

uint64_t
fm_ena_id_get(uint64_t ena)
{
	uint64_t id;

	switch (ENA_FORMAT(ena)) {
	case FM_ENA_FMT1:
		id = (ena & ENA_FMT1_ID_MASK) >> ENA_FMT1_ID_SHFT;
		break;
	case FM_ENA_FMT2:
		id = (ena & ENA_FMT2_ID_MASK) >> ENA_FMT2_ID_SHFT;
		break;
	default:
		id = 0;
	}

	return (id);
}

uint64_t
fm_ena_time_get(uint64_t ena)
{
	uint64_t time;

	switch (ENA_FORMAT(ena)) {
	case FM_ENA_FMT1:
		time = (ena & ENA_FMT1_TIME_MASK) >> ENA_FMT1_TIME_SHFT;
		break;
	case FM_ENA_FMT2:
		time = (ena & ENA_FMT2_TIME_MASK) >> ENA_FMT2_TIME_SHFT;
		break;
	default:
		time = 0;
	}

	return (time);
}

#ifdef _KERNEL
/*
 * Helper function to increment ereport dropped count.  Used by the event
 * rate limiting code to give feedback to the user about how many events were
 * rate limited by including them in the 'dropped' count.
 */
void
fm_erpt_dropped_increment(void)
{
	atomic_inc_64(&ratelimit_dropped);
}

void
fm_init(void)
{
	zevent_len_cur = 0;
	zevent_flags = 0;

	/* Initialize zevent allocation and generation kstats */
	fm_ksp = kstat_create("zfs", 0, "fm", "misc", KSTAT_TYPE_NAMED,
	    sizeof (struct erpt_kstat) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);

	if (fm_ksp != NULL) {
		fm_ksp->ks_data = &erpt_kstat_data;
		kstat_install(fm_ksp);
	} else {
		cmn_err(CE_NOTE, "failed to create fm/misc kstat\n");
	}

	mutex_init(&zevent_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zevent_list, sizeof (zevent_t),
	    offsetof(zevent_t, ev_node));
	cv_init(&zevent_cv, NULL, CV_DEFAULT, NULL);

	zfs_ereport_init();
}

void
fm_fini(void)
{
	uint_t count;

	zfs_ereport_fini();

	zfs_zevent_drain_all(&count);

	mutex_enter(&zevent_lock);
	cv_broadcast(&zevent_cv);

	zevent_flags |= ZEVENT_SHUTDOWN;
	while (zevent_waiters > 0) {
		mutex_exit(&zevent_lock);
		kpreempt(KPREEMPT_SYNC);
		mutex_enter(&zevent_lock);
	}
	mutex_exit(&zevent_lock);

	cv_destroy(&zevent_cv);
	list_destroy(&zevent_list);
	mutex_destroy(&zevent_lock);

	if (fm_ksp != NULL) {
		kstat_delete(fm_ksp);
		fm_ksp = NULL;
	}
}

ZFS_MODULE_PARAM(zfs_zevent, zfs_zevent_, len_max, UINT, ZMOD_RW,
	"Max event queue length");

#endif /* _KERNEL */
