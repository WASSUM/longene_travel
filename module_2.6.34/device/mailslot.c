/*
 * mailslot.c
 *
 * Copyright (C) 2006  Insigma Co., Ltd
 *
 * This software has been developed while working on the Linux Unified Kernel
 * project (http://www.longene.org) in the Insigma Research Institute,  
 * which is a subdivision of Insigma Co., Ltd (http://www.insigma.com.cn).
 * 
 * The project is sponsored by Insigma Co., Ltd.
 *
 * The authors can be reached at linux@insigma.com.cn.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of  the GNU General  Public License as published by the
 * Free Software Foundation; either version 2 of the  License, or (at your
 * option) any later version.
 *
 * Revision History:
 *   Dec 2008 - Created.
 */
 
/*
 * mailslot.c:
 * Refered to Wine code
 */
#include "unistr.h"
#include "handle.h"
#include "file.h"

#ifdef CONFIG_UNIFIED_KERNEL

#define AF_UNIX     1   /* Unix domain sockets      */
#define PF_UNIX                    AF_UNIX
#define SOCK_DGRAM                 2
#define FILE_SYNCHRONOUS_IO_ALERT       0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020

struct mailslot
{
	struct object       obj;
	struct fd          *fd;
	int                 write_fd;
	unsigned int        max_msgsize;
	timeout_t           read_timeout;
	struct list_head    writers;
};

/* mailslot functions */
static void mailslot_dump(struct object*, int);
static struct fd *mailslot_get_fd(struct object *);
static unsigned int mailslot_map_access(struct object *obj, unsigned int access);
static struct object *mailslot_open_file(struct object *obj, unsigned int access,
								unsigned int sharing, unsigned int options);
static void mailslot_destroy(struct object *);

static const struct object_ops mailslot_ops =
{
	sizeof(struct mailslot),   /* size */
	mailslot_dump,             /* dump */
	no_get_type,               /* get_type */
	mailslot_get_fd,           /* get_fd */
	mailslot_map_access,       /* map_access */
	no_lookup_name,            /* lookup_name */
	mailslot_open_file,        /* open_file */
	fd_close_handle,           /* close_handle */
	mailslot_destroy,          /* destroy */

	default_fd_signaled,       /* signaled */
	no_satisfied,              /* satisfied */
	no_signal,                 /* signal */
	default_get_sd,            /* get_sd */
	default_set_sd             /* set_sd */
};

static enum server_fd_type mailslot_get_fd_type(struct fd *fd);
static void mailslot_queue_async(struct fd *fd, const async_data_t *data, int type, int count);

static const struct fd_ops mailslot_fd_ops =
{
	default_fd_get_poll_events, /* get_poll_events */
	default_poll_event,         /* poll_event */
	no_flush,                   /* flush */
	mailslot_get_fd_type,       /* get_fd_type */
	default_fd_ioctl,           /* ioctl */
	mailslot_queue_async,       /* queue_async */
	default_fd_reselect_async,  /* reselect_async */
	default_fd_cancel_async     /* cancel_async */
};


struct mail_writer
{
	struct object         obj;
	struct fd            *fd;
	struct mailslot      *mailslot;
	struct list_head      entry;
	unsigned int          access;
	unsigned int          sharing;
};

static void mail_writer_dump(struct object *obj, int verbose);
static struct fd *mail_writer_get_fd(struct object *obj);
static unsigned int mail_writer_map_access(struct object *obj, unsigned int access);
static void mail_writer_destroy(struct object *obj);

static const struct object_ops mail_writer_ops =
{
	sizeof(struct mail_writer), /* size */
	mail_writer_dump,           /* dump */
	no_get_type,                /* get_type */
	mail_writer_get_fd,         /* get_fd */
	mail_writer_map_access,     /* map_access */
	no_lookup_name,             /* lookup_name */
	no_open_file,               /* open_file */
	fd_close_handle,            /* close_handle */
	mail_writer_destroy,        /* destroy */

	NULL,                      /* signaled */
	NULL,              /* satisfied */
	no_signal,                 /* signal */
	default_get_sd,            /* get_sd */
	default_set_sd             /* set_sd */
};

static enum server_fd_type mail_writer_get_fd_type(struct fd *fd);

static const struct fd_ops mail_writer_fd_ops =
{
	default_fd_get_poll_events,  /* get_poll_events */
	default_poll_event,          /* poll_event */
	no_flush,                    /* flush */
	mail_writer_get_fd_type,     /* get_fd_type */
	default_fd_ioctl,            /* ioctl */
	default_fd_queue_async,      /* queue_async */
	default_fd_reselect_async,   /* reselect_async */
	default_fd_cancel_async      /* cancel_async */
};


struct mailslot_device
{
	struct object       obj;         /* object header */
	struct fd          *fd;          /* pseudo-fd for ioctls */
	struct namespace   *mailslots;   /* mailslot namespace */
};

static void mailslot_device_dump(struct object *obj, int verbose);
static struct object_type *mailslot_device_get_type(struct object *obj);
static struct fd *mailslot_device_get_fd(struct object *obj);
static struct object *mailslot_device_lookup_name(struct object *obj, struct unicode_str *name,
										unsigned int attr);
static struct object *mailslot_device_open_file(struct object *obj, unsigned int access,
										unsigned int sharing, unsigned int options);
static void mailslot_device_destroy(struct object *obj);
static enum server_fd_type mailslot_device_get_fd_type(struct fd *fd);

static const struct object_ops mailslot_device_ops =
{
	sizeof(struct mailslot_device), /* size */
	mailslot_device_dump,           /* dump */
	mailslot_device_get_type,       /* get_type */
	mailslot_device_get_fd,         /* get_fd */
	no_map_access,                  /* map_access */
	mailslot_device_lookup_name,    /* lookup_name */
	mailslot_device_open_file,      /* open_file */
	fd_close_handle,                /* close_handle */
	mailslot_device_destroy,        /* destroy */

	NULL,                      /* signaled */
	no_satisfied,              /* satisfied */
	no_signal,                 /* signal */
	default_get_sd,            /* get_sd */
	default_set_sd             /* set_sd */
};

static const struct fd_ops mailslot_device_fd_ops =
{
	default_fd_get_poll_events,     /* get_poll_events */
	default_poll_event,             /* poll_event */
	no_flush,                       /* flush */
	mailslot_device_get_fd_type,    /* get_fd_type */
	default_fd_ioctl,               /* ioctl */
	default_fd_queue_async,         /* queue_async */
	default_fd_reselect_async,      /* reselect_async */
	default_fd_cancel_async         /* cancel_async */
};

static WCHAR mailslot_name[] = {'M','a','i','l','s','l','o','t',0};
static WCHAR mailslot_writer_name[] = {'M','a','i','l','s','l','o','t','_','W','r','i','t','e','r',0};
static WCHAR mailslot_device_name[] = {'M','a','i','l','s','l','o','t','_','D','e','v','i','c','e',0};

POBJECT_TYPE mailslot_object_type = NULL;
EXPORT_SYMBOL(mailslot_object_type);

POBJECT_TYPE mailslot_writer_object_type = NULL;
EXPORT_SYMBOL(mailslot_writer_object_type);

POBJECT_TYPE mailslot_device_object_type = NULL;
EXPORT_SYMBOL(mailslot_device_object_type);

static GENERIC_MAPPING mapping =
{
	STANDARD_RIGHTS_READ | SYNCHRONIZE | 0x1 /* QUERY_STATE */,
	STANDARD_RIGHTS_WRITE | SYNCHRONIZE | 0x2 /* MODIFY_STATE */,
	STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE | 0x1 /* QUERY_STATE */,
	STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3
};

VOID
init_mailslot_implement(VOID)
{
	OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
	UNICODE_STRING Name;

	memset(&ObjectTypeInitializer, 0, sizeof(ObjectTypeInitializer));
	init_unistr(&Name, mailslot_name);
	ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
	ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(struct mailslot);
	ObjectTypeInitializer.GenericMapping = mapping;
	ObjectTypeInitializer.PoolType = NonPagedPool;
	ObjectTypeInitializer.ValidAccessMask = EVENT_ALL_ACCESS;
	ObjectTypeInitializer.UseDefaultObject = TRUE;
	create_type_object(&ObjectTypeInitializer, &Name, &mailslot_object_type);
}

VOID
init_mailslot_writer_implement(VOID)
{
	OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
	UNICODE_STRING Name;

	memset(&ObjectTypeInitializer, 0, sizeof(ObjectTypeInitializer));
	init_unistr(&Name, mailslot_writer_name);
	ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
	ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(struct mail_writer);
	ObjectTypeInitializer.GenericMapping = mapping;
	ObjectTypeInitializer.PoolType = NonPagedPool;
	ObjectTypeInitializer.ValidAccessMask = EVENT_ALL_ACCESS;
	ObjectTypeInitializer.UseDefaultObject = TRUE;
	create_type_object(&ObjectTypeInitializer, &Name, &mailslot_writer_object_type);
}

VOID
init_mailslot_device_implement(VOID)
{
	OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
	UNICODE_STRING Name;

	memset(&ObjectTypeInitializer, 0, sizeof(ObjectTypeInitializer));
	init_unistr(&Name, mailslot_device_name);
	ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
	ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(struct mailslot_device);
	ObjectTypeInitializer.GenericMapping = mapping;
	ObjectTypeInitializer.PoolType = NonPagedPool;
	ObjectTypeInitializer.ValidAccessMask = EVENT_ALL_ACCESS;
	ObjectTypeInitializer.UseDefaultObject = TRUE;
	create_type_object(&ObjectTypeInitializer, &Name, &mailslot_device_object_type);
}

static void mailslot_destroy(struct object *obj)
{
	struct mailslot *mailslot = (struct mailslot *) obj;

	if (mailslot->write_fd != -1) {
		shutdown(mailslot->write_fd, SHUT_RDWR);
		close(mailslot->write_fd);
	}
	release_object(mailslot->fd);
}

static void mailslot_dump(struct object *obj, int verbose)
{
}

static enum server_fd_type mailslot_get_fd_type(struct fd *fd)
{
	return FD_TYPE_MAILSLOT;
}

static struct fd *mailslot_get_fd(struct object *obj)
{
	struct mailslot *mailslot = (struct mailslot *) obj;

	return (struct fd *)grab_object(mailslot->fd);
}

static unsigned int mailslot_map_access(struct object *obj, unsigned int access)
{
	/* mailslots can only be read */
	if (access & GENERIC_READ)
		access |= FILE_GENERIC_READ;
	if (access & GENERIC_ALL)
		access |= FILE_GENERIC_READ;
	return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static struct object *mailslot_open_file(struct object *obj, unsigned int access,
									unsigned int sharing, unsigned int options)
{
	struct mailslot *mailslot = (struct mailslot *)obj;
	struct mail_writer *writer;
	int unix_fd;
	NTSTATUS status;

	if (!(sharing & FILE_SHARE_READ)) {
		set_error(STATUS_SHARING_VIOLATION);
		return NULL;
	}

	if (!list_empty(&mailslot->writers)) {
		/* Readers and writers cannot be mixed.
		 * If there's more than one writer, all writers must open with FILE_SHARE_WRITE
		 */
		writer = LIST_ENTRY(list_head(&mailslot->writers), struct mail_writer, entry);

		if (((access & (GENERIC_WRITE|FILE_WRITE_DATA)) || (writer->access & FILE_WRITE_DATA)) &&
				!((sharing & FILE_SHARE_WRITE) && (writer->sharing & FILE_SHARE_WRITE))) {
			set_error(STATUS_SHARING_VIOLATION);
			return NULL;
		}
	}

	if ((unix_fd = dup(mailslot->write_fd)) < 0) {
		errno2ntstatus(-unix_fd);
		return NULL;
	}

	status = create_object(KernelMode,
			mailslot_object_type,
			NULL /* obj_attr*/,
			KernelMode,
			NULL,
			sizeof(struct mailslot),
			0,
			0,
			(PVOID *)&writer);

	if (NT_SUCCESS(status) && writer) {
		INIT_DISP_HEADER(&mailslot->obj.header, MAILSLOT,
				sizeof(struct mailslot) / sizeof(ULONG), 0);
		BODY_TO_HEADER(&writer->obj)->ops = &mail_writer_ops;
	}
	else {
		close(unix_fd);
		return NULL;
	}
	grab_object(mailslot);
	writer->mailslot = mailslot;
	writer->access   = mail_writer_map_access(&writer->obj, access);
	writer->sharing  = sharing;
	list_add_head(&mailslot->writers, &writer->entry);

	if (!(writer->fd = create_anonymous_fd(&mail_writer_fd_ops, unix_fd, &writer->obj, options))) {
		release_object(writer);
		return NULL;
	}
	return &writer->obj;
}

static void mailslot_queue_async(struct fd *fd, const async_data_t *data, int type, int count)
{
	struct mailslot *mailslot = get_fd_user(fd);
	struct async *async;

	if ((async = fd_queue_async(fd, data, type, count))) {
		async_set_timeout(async, mailslot->read_timeout ? mailslot->read_timeout : -1,
				STATUS_IO_TIMEOUT);
		release_object(async);
		set_error(STATUS_PENDING);
	}
}

static void mailslot_device_dump(struct object *obj, int verbose)
{
}

struct object_type *get_object_type(const struct unicode_str *name);

static struct object_type *mailslot_device_get_type(struct object *obj)
{
	static const WCHAR name[] = {'D','e','v','i','c','e'};
	static const struct unicode_str str = {name, sizeof(name)};
	return get_object_type(&str);
}

static struct fd *mailslot_device_get_fd(struct object *obj)
{
	struct mailslot_device *device = (struct mailslot_device *)obj;
	return (struct fd *)grab_object(device->fd);
}

static struct object *mailslot_device_lookup_name(struct object *obj, struct unicode_str *name,
											unsigned int attr)
{
	struct mailslot_device *device = (struct mailslot_device*)obj;
	struct object *found;

	if ((found = find_object(device->mailslots, name, attr | OBJ_CASE_INSENSITIVE)))
		name->len = 0;

	return found;
}

static struct object *mailslot_device_open_file(struct object *obj, unsigned int access,
											unsigned int sharing, unsigned int options)
{
	return grab_object(obj);
}

static void mailslot_device_destroy(struct object *obj)
{
	struct mailslot_device *device = (struct mailslot_device*)obj;

	if (device->fd)
		release_object(device->fd);
	free(device->mailslots);
}

static enum server_fd_type mailslot_device_get_fd_type(struct fd *fd)
{
	return FD_TYPE_DEVICE;
}

void create_mailslot_device(struct directory *root, const struct unicode_str *name)
{
	struct mailslot_device *dev;

	if ((dev = create_named_object_dir(root, name, 0, &mailslot_device_ops)) &&
			get_error() != STATUS_OBJECT_NAME_EXISTS) {
		dev->mailslots = NULL;
		if (!(dev->fd = alloc_pseudo_fd(&mailslot_device_fd_ops, &dev->obj, 0)) ||
				!(dev->mailslots = create_namespace(7))) {
			release_object(dev);
			dev = NULL;
		}
	}
	if (dev)
		make_object_static(&dev->obj);
}

static struct mailslot *create_mailslot(HANDLE root,
								const struct unicode_str *name, unsigned int attr,
								int max_msgsize, timeout_t read_timeout)
{
	struct object *obj;
	struct unicode_str new_name;
	struct mailslot_device *dev;
	struct mailslot *mailslot;
	int fds[2];
	NTSTATUS status;

	if (!name || !name->len) {
		status = create_object(KernelMode,
				mailslot_object_type,
				NULL /* obj_attr*/,
				KernelMode,
				NULL,
				sizeof(struct mailslot),
				0,
				0,
				(PVOID *)&mailslot);

		if (NT_SUCCESS(status) && mailslot) {
			INIT_DISP_HEADER(&mailslot->obj.header, MAILSLOT,
					sizeof(struct mailslot) / sizeof(ULONG), 0);
			BODY_TO_HEADER(&mailslot->obj)->ops = &mailslot_ops;
		}
		return mailslot;
	}

	if (!(obj = find_object_dir(root, name, attr, &new_name))) {
		set_error(STATUS_OBJECT_NAME_INVALID);
		return NULL;
	}

	if (!new_name.len) {
		if (attr & OBJ_OPENIF && BODY_TO_HEADER(obj)->ops == &mailslot_ops)
			/* it already exists - there can only be one mailslot to read from */
			set_error(STATUS_OBJECT_NAME_EXISTS);
		else if (attr & OBJ_OPENIF)
			set_error(STATUS_OBJECT_TYPE_MISMATCH);
		else
			set_error(STATUS_OBJECT_NAME_COLLISION);
		release_object(obj);
		return NULL;
	}

	if (BODY_TO_HEADER(obj)->ops != &mailslot_device_ops) {
		set_error(STATUS_OBJECT_NAME_INVALID);
		release_object(obj);
		return NULL;
	}

	dev = (struct mailslot_device *)obj;
	mailslot = create_wine_object(dev->mailslots, &mailslot_ops, &new_name, NULL);
	release_object(dev);

	if (!mailslot)
		return NULL;

	mailslot->fd = NULL;
	mailslot->write_fd = -1;
	mailslot->max_msgsize = max_msgsize;
	mailslot->read_timeout = read_timeout;
	INIT_LIST_HEAD(&mailslot->writers);

	if (socketpair(PF_UNIX, SOCK_DGRAM, 0, fds) >= 0) {
		fcntl(fds[0], F_SETFL, O_NONBLOCK);
		fcntl(fds[1], F_SETFL, O_NONBLOCK);
		shutdown(fds[0], SHUT_RD);
		mailslot->write_fd = fds[0];
		mailslot->fd = create_anonymous_fd(&mailslot_fd_ops, fds[1], &mailslot->obj,
				FILE_SYNCHRONOUS_IO_NONALERT);
		if (mailslot->fd)
			return mailslot;
	}

	release_object(mailslot);
	return NULL;
}

static void mail_writer_dump(struct object *obj, int verbose)
{
}

static void mail_writer_destroy(struct object *obj)
{
	struct mail_writer *writer = (struct mail_writer *) obj;

	if (writer->fd)
		release_object(writer->fd);
	list_remove(&writer->entry);
	release_object(writer->mailslot);
}

static enum server_fd_type mail_writer_get_fd_type(struct fd *fd)
{
	return FD_TYPE_MAILSLOT;
}

static struct fd *mail_writer_get_fd(struct object *obj)
{
	struct mail_writer *writer = (struct mail_writer *) obj;
	return (struct fd *)grab_object(writer->fd);
}

static unsigned int mail_writer_map_access(struct object *obj, unsigned int access)
{
    /* mailslot writers can only get write access */
	if (access & GENERIC_WRITE)
		access |= FILE_GENERIC_WRITE;
	if (access & GENERIC_ALL)
		access |= FILE_GENERIC_WRITE;
	return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static struct mailslot *get_mailslot_obj(struct w32process *process, obj_handle_t handle,
									unsigned int access)
{
	return (struct mailslot *)get_wine_handle_obj(process, handle, access, &mailslot_ops);
}

/* create a mailslot */
DECL_HANDLER(create_mailslot)
{
	struct mailslot *mailslot;
	struct unicode_str name;

	ktrace("\n");
	reply->handle = 0;
	get_req_unicode_str(&name);

	if ((mailslot = create_mailslot(req->rootdir, &name, req->attributes, req->max_msgsize,
					req->read_timeout))) {
		reply->handle = alloc_handle(get_current_w32process(), mailslot, req->access, req->attributes);
		release_object(mailslot);
	}
}


/* set mailslot information */
DECL_HANDLER(set_mailslot_info)
{
	struct mailslot *mailslot = get_mailslot_obj(get_current_w32process(), req->handle, 0);

	ktrace("\n");
	if (mailslot) {
		if (req->flags & MAILSLOT_SET_READ_TIMEOUT)
			mailslot->read_timeout = req->read_timeout;
		reply->max_msgsize = mailslot->max_msgsize;
		reply->read_timeout = mailslot->read_timeout;
		release_object(mailslot);
	}
}
#endif /* CONFIG_UNIFIED_KERNEL */
