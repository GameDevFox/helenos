/*
 * Copyright (c) 2008 Jakub Jermar
 * Copyright (c) 2011 Martin Sucha
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup fs
 * @{
 */ 

/**
 * @file	ext2fs_ops.c
 * @brief	Implementation of VFS operations for the EXT2 file system server.
 */

#include "ext2fs.h"
#include "../../vfs/vfs.h"
#include <libfs.h>
#include <libblock.h>
#include <libext2.h>
#include <ipc/services.h>
#include <ipc/devmap.h>
#include <macros.h>
#include <async.h>
#include <errno.h>
#include <str.h>
#include <byteorder.h>
#include <adt/hash_table.h>
#include <adt/list.h>
#include <assert.h>
#include <fibril_synch.h>
#include <sys/mman.h>
#include <align.h>
#include <adt/hash_table.h>
#include <sys/typefmt.h>
#include <malloc.h>
#include <stdio.h>
#include <inttypes.h>

#define EXT2FS_NODE(node)	((node) ? (ext2fs_node_t *) (node)->data : NULL)
#define EXT2FS_DBG(format, ...) {if (false) printf("ext2fs: %s: " format "\n", __FUNCTION__, ##__VA_ARGS__);}
#define OPEN_NODES_KEYS 2
#define OPEN_NODES_DEV_HANDLE_KEY 0
#define OPEN_NODES_INODE_KEY 1
#define OPEN_NODES_BUCKETS 256

typedef struct ext2fs_instance {
	link_t link;
	devmap_handle_t devmap_handle;
	ext2_filesystem_t *filesystem;
	unsigned int open_nodes_count;
} ext2fs_instance_t;

typedef struct ext2fs_node {
	ext2fs_instance_t *instance;
	ext2_inode_ref_t *inode_ref;
	fs_node_t *fs_node;
	link_t link;
	unsigned int references;
} ext2fs_node_t;

/*
 * Forward declarations of auxiliary functions
 */
static int ext2fs_instance_get(devmap_handle_t, ext2fs_instance_t **);
static void ext2fs_read_directory(ipc_callid_t, ipc_callid_t, aoff64_t,
	size_t, ext2fs_instance_t *, ext2_inode_ref_t *);
static void ext2fs_read_file(ipc_callid_t, ipc_callid_t, aoff64_t,
	size_t, ext2fs_instance_t *, ext2_inode_ref_t *);
static bool ext2fs_is_dots(const uint8_t *, size_t);
static int ext2fs_node_get_core(fs_node_t **, ext2fs_instance_t *, fs_index_t);
static int ext2fs_node_put_core(ext2fs_node_t *);

/*
 * Forward declarations of EXT2 libfs operations.
 */
static int ext2fs_root_get(fs_node_t **, devmap_handle_t);
static int ext2fs_match(fs_node_t **, fs_node_t *, const char *);
static int ext2fs_node_get(fs_node_t **, devmap_handle_t, fs_index_t);
static int ext2fs_node_open(fs_node_t *);
static int ext2fs_node_put(fs_node_t *);
static int ext2fs_create_node(fs_node_t **, devmap_handle_t, int);
static int ext2fs_destroy_node(fs_node_t *);
static int ext2fs_link(fs_node_t *, fs_node_t *, const char *);
static int ext2fs_unlink(fs_node_t *, fs_node_t *, const char *);
static int ext2fs_has_children(bool *, fs_node_t *);
static fs_index_t ext2fs_index_get(fs_node_t *);
static aoff64_t ext2fs_size_get(fs_node_t *);
static unsigned ext2fs_lnkcnt_get(fs_node_t *);
static char ext2fs_plb_get_char(unsigned);
static bool ext2fs_is_directory(fs_node_t *);
static bool ext2fs_is_file(fs_node_t *node);
static devmap_handle_t ext2fs_device_get(fs_node_t *node);

/*
 * Static variables
 */
static LIST_INITIALIZE(instance_list);
static FIBRIL_MUTEX_INITIALIZE(instance_list_mutex);
static hash_table_t open_nodes;
static FIBRIL_MUTEX_INITIALIZE(open_nodes_lock);

/* Hash table interface for open nodes hash table */
static hash_index_t open_nodes_hash(unsigned long key[])
{
	/* TODO: This is very simple and probably can be improved */
	return key[OPEN_NODES_INODE_KEY] % OPEN_NODES_BUCKETS;
}

static int open_nodes_compare(unsigned long key[], hash_count_t keys, 
    link_t *item)
{
	ext2fs_node_t *enode = hash_table_get_instance(item, ext2fs_node_t, link);
	assert(keys > 0);
	if (enode->instance->devmap_handle !=
	    ((devmap_handle_t) key[OPEN_NODES_DEV_HANDLE_KEY])) {
		return false;
	}
	if (keys == 1) {
		return true;
	}
	assert(keys == 2);
	return (enode->inode_ref->index == key[OPEN_NODES_INODE_KEY]);
}

static void open_nodes_remove_cb(link_t *link)
{
	/* We don't use remove callback for this hash table */
}

static hash_table_operations_t open_nodes_ops = {
	.hash = open_nodes_hash,
	.compare = open_nodes_compare,
	.remove_callback = open_nodes_remove_cb,
};

/**
 * 
 */
int ext2fs_global_init(void)
{
	if (!hash_table_create(&open_nodes, OPEN_NODES_BUCKETS,
	    OPEN_NODES_KEYS, &open_nodes_ops)) {
		return ENOMEM;
	}
	return EOK;
}

int ext2fs_global_fini(void)
{
	hash_table_destroy(&open_nodes);
	return EOK;
}


/*
 * EXT2 libfs operations.
 */

/**
 * Find an instance of filesystem for the given devmap_handle
 */
int ext2fs_instance_get(devmap_handle_t devmap_handle, ext2fs_instance_t **inst)
{
	EXT2FS_DBG("(%" PRIun ", -)", devmap_handle);
	ext2fs_instance_t *tmp;
	
	fibril_mutex_lock(&instance_list_mutex);

	if (list_empty(&instance_list)) {
		EXT2FS_DBG("list empty");
		fibril_mutex_unlock(&instance_list_mutex);
		return EINVAL;
	}

	list_foreach(instance_list, link) {
		tmp = list_get_instance(link, ext2fs_instance_t, link);
		
		if (tmp->devmap_handle == devmap_handle) {
			*inst = tmp;
			fibril_mutex_unlock(&instance_list_mutex);
			return EOK;
		}
	}
	
	EXT2FS_DBG("not found");
	
	fibril_mutex_unlock(&instance_list_mutex);
	return EINVAL;
}



int ext2fs_root_get(fs_node_t **rfn, devmap_handle_t devmap_handle)
{
	EXT2FS_DBG("(-, %" PRIun ")", devmap_handle);
	return ext2fs_node_get(rfn, devmap_handle, EXT2_INODE_ROOT_INDEX);
}

int ext2fs_match(fs_node_t **rfn, fs_node_t *pfn, const char *component)
{
	EXT2FS_DBG("(-,-,%s)", component);
	ext2fs_node_t *eparent = EXT2FS_NODE(pfn);
	ext2_filesystem_t *fs;
	ext2_directory_iterator_t it;
	int rc;
	size_t name_size;
	size_t component_size;
	bool found = false;
	uint32_t inode;
	
	fs = eparent->instance->filesystem;
	
	if (!ext2_inode_is_type(fs->superblock, eparent->inode_ref->inode,
	    EXT2_INODE_MODE_DIRECTORY)) {
		return ENOTDIR;
	}
	
	rc = ext2_directory_iterator_init(&it, fs, eparent->inode_ref);
	if (rc != EOK) {
		return rc;
	}

	/* Find length of component in bytes
	 * TODO: check for library function call that does this
	 */
	component_size = 0;
	while (*(component+component_size) != 0) {
		component_size++;
	}
	
	while (it.current != NULL) {
		inode = ext2_directory_entry_ll_get_inode(it.current);
		
		/* ignore empty directory entries */
		if (inode != 0) {
			name_size = ext2_directory_entry_ll_get_name_length(fs->superblock,
				it.current);

			if (name_size == component_size && bcmp(component, &it.current->name,
				    name_size) == 0) {
				rc = ext2fs_node_get_core(rfn, eparent->instance,
					inode);
				if (rc != EOK) {
					ext2_directory_iterator_fini(&it);
					return rc;
				}
				found = true;
				break;
			}
		}
		
		rc = ext2_directory_iterator_next(&it);
		if (rc != EOK) {
			ext2_directory_iterator_fini(&it);
			return rc;
		}
	}
	
	ext2_directory_iterator_fini(&it);
	
	if (!found) {
		return ENOENT;
	}
	
	return EOK;
}

/** Instantiate a EXT2 in-core node. */
int ext2fs_node_get(fs_node_t **rfn, devmap_handle_t devmap_handle, fs_index_t index)
{
	EXT2FS_DBG("(-,%" PRIun ",%u)", devmap_handle, index);
	
	ext2fs_instance_t *inst = NULL;
	int rc;
	
	rc = ext2fs_instance_get(devmap_handle, &inst);
	if (rc != EOK) {
		return rc;
	}
	
	return ext2fs_node_get_core(rfn, inst, index);
}

int ext2fs_node_get_core(fs_node_t **rfn, ext2fs_instance_t *inst,
		fs_index_t index)
{
	int rc;
	fs_node_t *node = NULL;
	ext2fs_node_t *enode = NULL;
	
	ext2_inode_ref_t *inode_ref = NULL;

	fibril_mutex_lock(&open_nodes_lock);
	
	/* Check if the node is not already open */
	unsigned long key[] = {
		[OPEN_NODES_DEV_HANDLE_KEY] = inst->devmap_handle,
		[OPEN_NODES_INODE_KEY] = index,
	};
	link_t *already_open = hash_table_find(&open_nodes, key);

	if (already_open) {
		enode = hash_table_get_instance(already_open, ext2fs_node_t, link);
		*rfn = enode->fs_node;
		enode->references++;

		fibril_mutex_unlock(&open_nodes_lock);
		return EOK;
	}

	enode = malloc(sizeof(ext2fs_node_t));
	if (enode == NULL) {
		fibril_mutex_unlock(&open_nodes_lock);
		return ENOMEM;
	}

	node = malloc(sizeof(fs_node_t));
	if (node == NULL) {
		free(enode);
		fibril_mutex_unlock(&open_nodes_lock);
		return ENOMEM;
	}	
	fs_node_initialize(node);

	rc = ext2_filesystem_get_inode_ref(inst->filesystem, index, &inode_ref);
	if (rc != EOK) {
		free(enode);
		free(node);
		fibril_mutex_unlock(&open_nodes_lock);
		return rc;
	}
	
	enode->inode_ref = inode_ref;
	enode->instance = inst;
	enode->references = 1;
	enode->fs_node = node;
	link_initialize(&enode->link);
	
	node->data = enode;
	*rfn = node;
	
	hash_table_insert(&open_nodes, key, &enode->link);
	inst->open_nodes_count++;
	
	EXT2FS_DBG("inode: %u", inode_ref->index);
	
	EXT2FS_DBG("EOK");

	fibril_mutex_unlock(&open_nodes_lock);
	return EOK;
}

int ext2fs_node_open(fs_node_t *fn)
{
	EXT2FS_DBG("");
	/*
	 * Opening a file is stateless, nothing
	 * to be done here.
	 */
	return EOK;
}

int ext2fs_node_put(fs_node_t *fn)
{
	EXT2FS_DBG("");
	int rc;
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	
	fibril_mutex_lock(&open_nodes_lock);

	assert(enode->references > 0);
	enode->references--;
	if (enode->references == 0) {
		rc = ext2fs_node_put_core(enode);
		if (rc != EOK) {
			fibril_mutex_unlock(&open_nodes_lock);
			return rc;
		}
	}

	fibril_mutex_unlock(&open_nodes_lock);
	
	return EOK;
}

int ext2fs_node_put_core(ext2fs_node_t *enode)
{
	int rc;

	unsigned long key[] = {
		[OPEN_NODES_DEV_HANDLE_KEY] = enode->instance->devmap_handle,
		[OPEN_NODES_INODE_KEY] = enode->inode_ref->index,
	};
	hash_table_remove(&open_nodes, key, OPEN_NODES_KEYS);
	assert(enode->instance->open_nodes_count > 0);
	enode->instance->open_nodes_count--;

	rc = ext2_filesystem_put_inode_ref(enode->inode_ref);
	if (rc != EOK) {
		EXT2FS_DBG("ext2_filesystem_put_inode_ref failed");
		return rc;
	}

	free(enode->fs_node);
	free(enode);
	return EOK;
}

int ext2fs_create_node(fs_node_t **rfn, devmap_handle_t devmap_handle, int flags)
{
	EXT2FS_DBG("");
	// TODO
	return ENOTSUP;
}

int ext2fs_destroy_node(fs_node_t *fn)
{
	EXT2FS_DBG("");
	// TODO
	return ENOTSUP;
}

int ext2fs_link(fs_node_t *pfn, fs_node_t *cfn, const char *name)
{
	EXT2FS_DBG("");
	// TODO
	return ENOTSUP;
}

int ext2fs_unlink(fs_node_t *pfn, fs_node_t *cfn, const char *nm)
{
	EXT2FS_DBG("");
	// TODO
	return ENOTSUP;
}

int ext2fs_has_children(bool *has_children, fs_node_t *fn)
{
	EXT2FS_DBG("");
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	ext2_directory_iterator_t it;
	ext2_filesystem_t *fs;
	int rc;
	bool found = false;
	size_t name_size;

	fs = enode->instance->filesystem;

	if (!ext2_inode_is_type(fs->superblock, enode->inode_ref->inode,
	    EXT2_INODE_MODE_DIRECTORY)) {
		*has_children = false;
		EXT2FS_DBG("EOK - false");
		return EOK;
	}
	
	rc = ext2_directory_iterator_init(&it, fs, enode->inode_ref);
	if (rc != EOK) {
		EXT2FS_DBG("error %u", rc);
		return rc;
	}
	
	/* Find a non-empty directory entry */
	while (it.current != NULL) {
		if (it.current->inode != 0) {
			name_size = ext2_directory_entry_ll_get_name_length(fs->superblock,
				it.current);
			if (!ext2fs_is_dots(&it.current->name, name_size)) {
				found = true;
				break;
			}
		}
		
		rc = ext2_directory_iterator_next(&it);
		if (rc != EOK) {
			ext2_directory_iterator_fini(&it);
			EXT2FS_DBG("error %u", rc);
			return rc;
		}
	}
	
	rc = ext2_directory_iterator_fini(&it);
	if (rc != EOK) {
		EXT2FS_DBG("error %u", rc);
		return rc;
	}

	*has_children = found;
	EXT2FS_DBG("EOK");
	
	return EOK;
}


fs_index_t ext2fs_index_get(fs_node_t *fn)
{
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	EXT2FS_DBG("%u", enode->inode_ref->index);
	return enode->inode_ref->index;
}

aoff64_t ext2fs_size_get(fs_node_t *fn)
{
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	aoff64_t size = ext2_inode_get_size(enode->instance->filesystem->superblock,
	    enode->inode_ref->inode);
	EXT2FS_DBG("%" PRIu64, size);
	return size;
}

unsigned ext2fs_lnkcnt_get(fs_node_t *fn)
{
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	unsigned count = ext2_inode_get_usage_count(enode->inode_ref->inode);
	EXT2FS_DBG("%u", count);
	return count;
}

char ext2fs_plb_get_char(unsigned pos)
{
	return ext2fs_reg.plb_ro[pos % PLB_SIZE];
}

bool ext2fs_is_directory(fs_node_t *fn)
{
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	bool is_dir = ext2_inode_is_type(enode->instance->filesystem->superblock,
	    enode->inode_ref->inode, EXT2_INODE_MODE_DIRECTORY);
	EXT2FS_DBG("%s", is_dir ? "true" : "false");
	EXT2FS_DBG("%u", enode->inode_ref->index);
	return is_dir;
}

bool ext2fs_is_file(fs_node_t *fn)
{
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	bool is_file = ext2_inode_is_type(enode->instance->filesystem->superblock,
	    enode->inode_ref->inode, EXT2_INODE_MODE_FILE);
	EXT2FS_DBG("%s", is_file ? "true" : "false");
	return is_file;
}

devmap_handle_t ext2fs_device_get(fs_node_t *fn)
{
	EXT2FS_DBG("");
	ext2fs_node_t *enode = EXT2FS_NODE(fn);
	return enode->instance->devmap_handle;
}

/** libfs operations */
libfs_ops_t ext2fs_libfs_ops = {
	.root_get = ext2fs_root_get,
	.match = ext2fs_match,
	.node_get = ext2fs_node_get,
	.node_open = ext2fs_node_open,
	.node_put = ext2fs_node_put,
	.create = ext2fs_create_node,
	.destroy = ext2fs_destroy_node,
	.link = ext2fs_link,
	.unlink = ext2fs_unlink,
	.has_children = ext2fs_has_children,
	.index_get = ext2fs_index_get,
	.size_get = ext2fs_size_get,
	.lnkcnt_get = ext2fs_lnkcnt_get,
	.plb_get_char = ext2fs_plb_get_char,
	.is_directory = ext2fs_is_directory,
	.is_file = ext2fs_is_file,
	.device_get = ext2fs_device_get
};

/*
 * VFS operations.
 */

void ext2fs_mounted(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	int rc;
	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
	ext2_filesystem_t *fs;
	ext2fs_instance_t *inst;
	bool read_only;
	
	/* Accept the mount options */
	char *opts;
	rc = async_data_write_accept((void **) &opts, true, 0, 0, 0, NULL);
	
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}

	free(opts);
	
	/* Allocate libext2 filesystem structure */
	fs = (ext2_filesystem_t *) malloc(sizeof(ext2_filesystem_t));
	if (fs == NULL) {
		async_answer_0(rid, ENOMEM);
		return;
	}
	
	/* Allocate instance structure */
	inst = (ext2fs_instance_t *) malloc(sizeof(ext2fs_instance_t));
	if (inst == NULL) {
		free(fs);
		async_answer_0(rid, ENOMEM);
		return;
	}
	
	/* Initialize the filesystem  */
	rc = ext2_filesystem_init(fs, devmap_handle);
	if (rc != EOK) {
		free(fs);
		free(inst);
		async_answer_0(rid, rc);
		return;
	}
	
	/* Do some sanity checking */
	rc = ext2_filesystem_check_sanity(fs);
	if (rc != EOK) {
		ext2_filesystem_fini(fs);
		free(fs);
		free(inst);
		async_answer_0(rid, rc);
		return;
	}
	
	/* Check flags */
	rc = ext2_filesystem_check_flags(fs, &read_only);
	if (rc != EOK) {
		ext2_filesystem_fini(fs);
		free(fs);
		free(inst);
		async_answer_0(rid, rc);
		return;
	}
	
	/* Initialize instance */
	link_initialize(&inst->link);
	inst->devmap_handle = devmap_handle;
	inst->filesystem = fs;
	inst->open_nodes_count = 0;
	
	/* Read root node */
	fs_node_t *root_node;
	rc = ext2fs_node_get_core(&root_node, inst, EXT2_INODE_ROOT_INDEX);
	if (rc != EOK) {
		ext2_filesystem_fini(fs);
		free(fs);
		free(inst);
		async_answer_0(rid, rc);
		return;
	}
	ext2fs_node_t *enode = EXT2FS_NODE(root_node);
		
	/* Add instance to the list */
	fibril_mutex_lock(&instance_list_mutex);
	list_append(&inst->link, &instance_list);
	fibril_mutex_unlock(&instance_list_mutex);
	
	async_answer_3(rid, EOK,
	    EXT2_INODE_ROOT_INDEX,
	    0,
	    ext2_inode_get_usage_count(enode->inode_ref->inode));
	
	ext2fs_node_put(root_node);
}

void ext2fs_mount(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	libfs_mount(&ext2fs_libfs_ops, ext2fs_reg.fs_handle, rid, request);
}

void ext2fs_unmounted(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
	ext2fs_instance_t *inst;
	int rc;
	
	rc = ext2fs_instance_get(devmap_handle, &inst);
	
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
	
	fibril_mutex_lock(&open_nodes_lock);

	EXT2FS_DBG("open_nodes_count = %d", inst->open_nodes_count)
	if (inst->open_nodes_count != 0) {
		fibril_mutex_unlock(&open_nodes_lock);
		async_answer_0(rid, EBUSY);
		return;
	}
	
	/* Remove the instance from the list */
	fibril_mutex_lock(&instance_list_mutex);
	list_remove(&inst->link);
	fibril_mutex_unlock(&instance_list_mutex);

	fibril_mutex_unlock(&open_nodes_lock);
	
	ext2_filesystem_fini(inst->filesystem);
	
	async_answer_0(rid, EOK);
}

void ext2fs_unmount(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	libfs_unmount(&ext2fs_libfs_ops, rid, request);
}

void ext2fs_lookup(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	libfs_lookup(&ext2fs_libfs_ops, ext2fs_reg.fs_handle, rid, request);
}

void ext2fs_read(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
	fs_index_t index = (fs_index_t) IPC_GET_ARG2(*request);
	aoff64_t pos =
	    (aoff64_t) MERGE_LOUP32(IPC_GET_ARG3(*request), IPC_GET_ARG4(*request));
	
	ext2fs_instance_t *inst;
	ext2_inode_ref_t *inode_ref;
	int rc;
	
	/*
	 * Receive the read request.
	 */
	ipc_callid_t callid;
	size_t size;
	if (!async_data_read_receive(&callid, &size)) {
		async_answer_0(callid, EINVAL);
		async_answer_0(rid, EINVAL);
		return;
	}
	
	rc = ext2fs_instance_get(devmap_handle, &inst);
	if (rc != EOK) {
		async_answer_0(callid, rc);
		async_answer_0(rid, rc);
		return;
	}
	
	rc = ext2_filesystem_get_inode_ref(inst->filesystem, index, &inode_ref);
	if (rc != EOK) {
		async_answer_0(callid, rc);
		async_answer_0(rid, rc);
		return;
	}
	
	if (ext2_inode_is_type(inst->filesystem->superblock, inode_ref->inode,
		    EXT2_INODE_MODE_FILE)) {
		ext2fs_read_file(rid, callid, pos, size, inst, inode_ref);
	}
	else if (ext2_inode_is_type(inst->filesystem->superblock, inode_ref->inode,
		    EXT2_INODE_MODE_DIRECTORY)) {
		ext2fs_read_directory(rid, callid, pos, size, inst, inode_ref);
	}
	else {
		/* Other inode types not supported */
		async_answer_0(callid, ENOTSUP);
		async_answer_0(rid, ENOTSUP);
	}
	
	ext2_filesystem_put_inode_ref(inode_ref);
	
}

/**
 * Determine whether given directory entry name is . or ..
 */
bool ext2fs_is_dots(const uint8_t *name, size_t name_size) {
	if (name_size == 1 && name[0] == '.') {
		return true;
	}
	
	if (name_size == 2 && name[0] == '.' && name[1] == '.') {
		return true;
	}
	
	return false;
}

void ext2fs_read_directory(ipc_callid_t rid, ipc_callid_t callid, aoff64_t pos,
	size_t size, ext2fs_instance_t *inst, ext2_inode_ref_t *inode_ref)
{
	ext2_directory_iterator_t it;
	aoff64_t cur;
	uint8_t *buf;
	size_t name_size;
	int rc;
	bool found = false;
	
	rc = ext2_directory_iterator_init(&it, inst->filesystem, inode_ref);
	if (rc != EOK) {
		async_answer_0(callid, rc);
		async_answer_0(rid, rc);
		return;
	}
	
	/* Find the index we want to read
	 * Note that we need to iterate and count as
	 * the underlying structure is a linked list
	 * Moreover, we want to skip . and .. entries
	 * as these are not used in HelenOS
	 */
	cur = 0;
	while (it.current != NULL) {
		if (it.current->inode == 0) {
			goto skip;
		}
		
		name_size = ext2_directory_entry_ll_get_name_length(
			inst->filesystem->superblock, it.current);
		
		/* skip . and .. */
		if (ext2fs_is_dots(&it.current->name, name_size)) {
			goto skip;
		}
		
		/* Is this the dir entry we want to read? */
		if (cur == pos) {
			/* The on-disk entry does not contain \0 at the end
			 * end of entry name, so we copy it to new buffer
			 * and add the \0 at the end
			 */
			buf = malloc(name_size+1);
			if (buf == NULL) {
				ext2_directory_iterator_fini(&it);
				async_answer_0(callid, ENOMEM);
				async_answer_0(rid, ENOMEM);
				return;
			}
			memcpy(buf, &it.current->name, name_size);
			*(buf+name_size) = 0;
			found = true;
			(void) async_data_read_finalize(callid, buf, name_size+1);
			free(buf);
			break;
		}
		cur++;
		
skip:
		rc = ext2_directory_iterator_next(&it);
		if (rc != EOK) {
			ext2_directory_iterator_fini(&it);
			async_answer_0(callid, rc);
			async_answer_0(rid, rc);
			return;
		}
	}
	
	rc = ext2_directory_iterator_fini(&it);
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
	
	if (found) {
		async_answer_1(rid, EOK, 1);
	}
	else {
		async_answer_0(callid, ENOENT);
		async_answer_0(rid, ENOENT);
	}
}

void ext2fs_read_file(ipc_callid_t rid, ipc_callid_t callid, aoff64_t pos,
	size_t size, ext2fs_instance_t *inst, ext2_inode_ref_t *inode_ref)
{
	int rc;
	uint32_t block_size;
	aoff64_t file_block;
	uint64_t file_size;
	uint32_t fs_block;
	size_t offset_in_block;
	size_t bytes;
	block_t *block;
	uint8_t *buffer;
	
	file_size = ext2_inode_get_size(inst->filesystem->superblock,
		inode_ref->inode);
	
	if (pos >= file_size) {
		/* Read 0 bytes successfully */
		async_data_read_finalize(callid, NULL, 0);
		async_answer_1(rid, EOK, 0);
		return;
	}
	
	/* For now, we only read data from one block at a time */
	block_size = ext2_superblock_get_block_size(inst->filesystem->superblock);
	file_block = pos / block_size;
	offset_in_block = pos % block_size;
	bytes = min(block_size - offset_in_block, size);
	
	/* Handle end of file */
	if (pos + bytes > file_size) {
		bytes = file_size - pos;
	}
	
	/* Get the real block number */
	rc = ext2_filesystem_get_inode_data_block_index(inst->filesystem,
		inode_ref->inode, file_block, &fs_block);
	if (rc != EOK) {
		async_answer_0(callid, rc);
		async_answer_0(rid, rc);
		return;
	}
	
	/* Check for sparse file
	 * If ext2_filesystem_get_inode_data_block_index returned
	 * fs_block == 0, it means that the given block is not allocated for the 
	 * file and we need to return a buffer of zeros
	 */
	if (fs_block == 0) {
		buffer = malloc(bytes);
		if (buffer == NULL) {
			async_answer_0(callid, ENOMEM);
			async_answer_0(rid, ENOMEM);
			return;
		}
		
		memset(buffer, 0, bytes);
		
		async_data_read_finalize(callid, buffer, bytes);
		async_answer_1(rid, EOK, bytes);
		
		free(buffer);
		
		return;
	}
	
	/* Usual case - we need to read a block from device */
	rc = block_get(&block, inst->devmap_handle, fs_block, BLOCK_FLAGS_NONE);
	if (rc != EOK) {
		async_answer_0(callid, rc);
		async_answer_0(rid, rc);
		return;
	}
	
	assert(offset_in_block + bytes <= block_size);
	async_data_read_finalize(callid, block->data + offset_in_block, bytes);
	
	rc = block_put(block);
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
		
	async_answer_1(rid, EOK, bytes);
}

void ext2fs_write(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
//	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
//	fs_index_t index = (fs_index_t) IPC_GET_ARG2(*request);
//	aoff64_t pos =
//	    (aoff64_t) MERGE_LOUP32(IPC_GET_ARG3(*request), IPC_GET_ARG4(*request));
	
	// TODO
	async_answer_0(rid, ENOTSUP);
}

void ext2fs_truncate(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
//	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
//	fs_index_t index = (fs_index_t) IPC_GET_ARG2(*request);
//	aoff64_t size =
//	    (aoff64_t) MERGE_LOUP32(IPC_GET_ARG3(*request), IPC_GET_ARG4(*request));
	
	// TODO
	async_answer_0(rid, ENOTSUP);
}

void ext2fs_close(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	async_answer_0(rid, EOK);
}

void ext2fs_destroy(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
//	devmap_handle_t devmap_handle = (devmap_handle_t)IPC_GET_ARG1(*request);
//	fs_index_t index = (fs_index_t)IPC_GET_ARG2(*request);
	
	// TODO
	async_answer_0(rid, ENOTSUP);
}

void ext2fs_open_node(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	libfs_open_node(&ext2fs_libfs_ops, ext2fs_reg.fs_handle, rid, request);
}

void ext2fs_stat(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
	libfs_stat(&ext2fs_libfs_ops, ext2fs_reg.fs_handle, rid, request);
}

void ext2fs_sync(ipc_callid_t rid, ipc_call_t *request)
{
	EXT2FS_DBG("");
//	devmap_handle_t devmap_handle = (devmap_handle_t) IPC_GET_ARG1(*request);
//	fs_index_t index = (fs_index_t) IPC_GET_ARG2(*request);
	
	// TODO
	async_answer_0(rid, ENOTSUP);
}

/**
 * @}
 */