/*
 * Copyright (c) 2025 WANG Junyang (adong660@gmail.com)
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

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_inode_ops.c
 * @brief Optimized ext4 operations that work directly with inode references
 *        instead of path strings for better performance.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_oflags.h>
#include <ext4_debug.h>

#include <ext4.h>
#include <ext4_inode_ops.h>
#include <ext4_trans.h>
#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_dir.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_block_group.h>
#include <ext4_dir_idx.h>
#include <ext4_bcache.h>

#include <stdlib.h>
#include <string.h>

/********************************FILE OPERATIONS*****************************/

int ext4_inode_create_file(struct ext4_mountpoint *mp,
			   struct ext4_inode_ref *parent_ref,
			   const char *name, uint32_t name_len,
			   uint32_t mode, struct ext4_inode_ref *child_ref)
{
	int r;
	struct ext4_fs *fs;

	ext4_assert(mp && parent_ref && name && child_ref);

	if (name_len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	fs = &mp->fs;

	if (fs->read_only)
		return EROFS;

	/* Check if parent is a directory */
	if (!ext4_inode_is_type(&fs->sb, parent_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	ext4_block_cache_write_back(fs->bdev, 1);

	/* Start transaction */
	r = ext4_trans_start(mp);
	if (r != EOK)
		return r;

	/* Determine if we're creating a directory or not for allocation purposes */
	bool is_directory = (mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_DIRECTORY;
	int filetype = is_directory ? EXT4_DE_DIR : EXT4_DE_REG_FILE;

	/* Allocate new inode */
	r = ext4_fs_alloc_inode(fs, child_ref, filetype);
	if (r != EOK)
		goto Finish;

	/* Set the exact file mode specified by caller */
	ext4_inode_set_mode(&fs->sb, child_ref->inode, mode);
	child_ref->dirty = true;

	/* Initialize inode blocks */
	ext4_fs_inode_blocks_init(fs, child_ref);

	/* Link with parent directory */
	r = ext4_link(mp, parent_ref, child_ref, name, name_len, false);
	if (r != EOK) {
		/* Fail. Free new inode. */
		ext4_fs_free_inode(child_ref);
		/* We do not want to write new inode.
		   But block has to be released. */
		child_ref->dirty = false;
		ext4_fs_put_inode_ref(child_ref);
		goto Finish;
	}

Finish:
	if (r != EOK)
		ext4_trans_abort(mp);
	else
		ext4_trans_stop(mp);

	/* Workaround: Put and re-get inode references to flush data */
	uint32_t parent_index = parent_ref->index;
	uint32_t child_index = child_ref->index;

	ext4_fs_put_inode_ref(parent_ref);
	ext4_fs_put_inode_ref(child_ref);

	ext4_block_cache_write_back(fs->bdev, 0);
	ext4_block_cache_flush(fs->bdev);

	ext4_fs_get_inode_ref(fs, parent_index, parent_ref);
	ext4_fs_get_inode_ref(fs, child_index, child_ref);

	return r;
}

int ext4_inode_unlink(struct ext4_mountpoint *mp,
		      struct ext4_inode_ref *parent_ref,
		      const char *name, uint32_t name_len)
{
	int r;
	struct ext4_fs *fs;
	struct ext4_inode_ref child_ref;
	uint32_t child_inode;
	uint8_t child_type;
	bool is_dir;

	ext4_assert(mp && parent_ref && name);

	if (name_len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	fs = &mp->fs;

	if (fs->read_only)
		return EROFS;

	/* Check if parent is a directory */
	if (!ext4_inode_is_type(&fs->sb, parent_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	ext4_block_cache_write_back(fs->bdev, 1);

	/* Start transaction */
	r = ext4_trans_start(mp);
	if (r != EOK)
		return r;

	/* Find the child entry in the parent directory */
	r = ext4_inode_find_child(parent_ref, name, name_len, &child_inode, &child_type);
	if (r != EOK)
		goto Finish;

	/* Get the child inode reference */
	r = ext4_fs_get_inode_ref(fs, child_inode, &child_ref);
	if (r != EOK)
		goto Finish;

	/* Check if it's a directory and if it has children */
	is_dir = ext4_inode_is_type(&fs->sb, child_ref.inode, EXT4_INODE_MODE_DIRECTORY);
	if (is_dir) {
		bool has_children;
		r = ext4_has_children(&has_children, &child_ref);
		if (r != EOK) {
			ext4_fs_put_inode_ref(&child_ref);
			goto Finish;
		}

		/* Cannot unlink non-empty directory */
		if (has_children) {
			ext4_fs_put_inode_ref(&child_ref);
			r = ENOTEMPTY;
			goto Finish;
		}
	}

	/* If link count will be zero after unlinking, truncate the inode first */
	if (ext4_inode_get_links_cnt(child_ref.inode) == 1) {
		if (is_dir) {
			/* For directories, use ext4_trunc_dir */
			r = ext4_trunc_dir(mp, parent_ref, &child_ref);
		} else {
			/* For regular files, truncate to zero */
			r = ext4_trunc_inode(mp, child_ref.index, 0);
		}

		if (r != EOK) {
			ext4_fs_put_inode_ref(&child_ref);
			goto Finish;
		}
	}

	/* Remove the directory entry */
	r = ext4_unlink(mp, parent_ref, &child_ref, name, name_len);
	if (r != EOK) {
		ext4_fs_put_inode_ref(&child_ref);
		goto Finish;
	}

	/* If link count is now zero, free the inode */
	if (!ext4_inode_get_links_cnt(child_ref.inode)) {
		ext4_inode_set_del_time(child_ref.inode, -1L);

		r = ext4_fs_free_inode(&child_ref);
		if (r != EOK) {
			/* Put the reference but ignore errors since we're already failing */
			ext4_fs_put_inode_ref(&child_ref);
			goto Finish;
		}

		/* Note: ext4_fs_free_inode puts the inode reference for us */
	} else {
		/* Put the reference if not freed */
		r = ext4_fs_put_inode_ref(&child_ref);
		if (r != EOK)
			goto Finish;
	}

Finish:
	if (r != EOK)
		ext4_trans_abort(mp);
	else
		ext4_trans_stop(mp);

	/* Workaround: Put and re-get parent references to flush data */
	uint32_t parent_index = parent_ref->index;
	uint32_t child_index = child_ref.index;

	ext4_fs_put_inode_ref(parent_ref);
	ext4_fs_put_inode_ref(&child_ref);

	ext4_block_cache_write_back(fs->bdev, 0);
	ext4_block_cache_flush(fs->bdev);

	ext4_fs_get_inode_ref(fs, parent_index, parent_ref);
	ext4_fs_get_inode_ref(fs, child_index, &child_ref);

	return r;
}

int ext4_inode_find_child(struct ext4_inode_ref *parent_ref,
			  const char *name, uint32_t name_len,
			  uint32_t *child_inode, uint8_t *child_type)
{
	struct ext4_dir_search_result result;
	int r;

	ext4_assert(parent_ref && name && child_inode);

	/* Check if parent is a directory */
	if (!ext4_inode_is_type(&parent_ref->fs->sb, parent_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	r = ext4_dir_find_entry(&result, parent_ref, name, name_len);
	if (r != EOK)
		return r;

	*child_inode = ext4_dir_en_get_inode(result.dentry);

	if (child_type && ext4_sb_feature_incom(&parent_ref->fs->sb, EXT4_FINCOM_FILETYPE)) {
		*child_type = ext4_dir_en_get_inode_type(&parent_ref->fs->sb, result.dentry);
	}

	r = ext4_dir_destroy_result(parent_ref, &result);
	return r;
}

int ext4_inode_fopen(ext4_file *file, struct ext4_mountpoint *mp,
		     struct ext4_inode_ref *inode_ref, uint32_t flags)
{
	ext4_assert(file && mp && inode_ref);

	if (mp->fs.read_only && (flags & O_CREAT))
		return EROFS;

	file->mp = mp;
	file->fsize = ext4_inode_get_size(&mp->fs.sb, inode_ref->inode);
	file->inode = inode_ref->index;
	file->fpos = 0;
	file->flags = flags;

	if (flags & O_APPEND)
		file->fpos = file->fsize;

	return EOK;
}

int ext4_inode_rename(struct ext4_mountpoint *mp,
		      struct ext4_inode_ref *old_parent_ref,
		      const char *old_name, uint32_t old_name_len,
		      struct ext4_inode_ref *new_parent_ref,
		      const char *new_name, uint32_t new_name_len)
{
	int r;
	struct ext4_fs *fs;
	struct ext4_inode_ref child_ref;
	uint32_t child_inode;
	uint8_t child_type;
	uint32_t existing_inode;
	uint8_t existing_type;
	struct ext4_inode_ref existing_ref;

	ext4_assert(mp && old_parent_ref && old_name && new_parent_ref && new_name);

	if (old_name_len > EXT4_DIRECTORY_FILENAME_LEN || 
	    new_name_len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	fs = &mp->fs;

	if (fs->read_only)
		return EROFS;

	/* Check if both parents are directories */
	if (!ext4_inode_is_type(&fs->sb, old_parent_ref->inode, EXT4_INODE_MODE_DIRECTORY) ||
	    !ext4_inode_is_type(&fs->sb, new_parent_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	ext4_block_cache_write_back(fs->bdev, 1);

	/* Start transaction */
	r = ext4_trans_start(mp);
	if (r != EOK)
		return r;

	/* Find the source file */
	r = ext4_inode_find_child(old_parent_ref, old_name, old_name_len, &child_inode, &child_type);
	if (r != EOK)
		goto Finish;

	/* Get the source file inode reference */
	r = ext4_fs_get_inode_ref(fs, child_inode, &child_ref);
	if (r != EOK)
		goto Finish;

	/* Check if target already exists */
	r = ext4_inode_find_child(new_parent_ref, new_name, new_name_len, &existing_inode, &existing_type);
	if (r == EOK) {
		/* Target exists - check if it's the same file */
		if (existing_inode == child_inode) {
			/* Same file, nothing to do */
			ext4_fs_put_inode_ref(&child_ref);
			goto Success;
		}

		/* Different file exists - remove it first */
		r = ext4_fs_get_inode_ref(fs, existing_inode, &existing_ref);
		if (r != EOK) {
			ext4_fs_put_inode_ref(&child_ref);
			goto Finish;
		}

		/* Check type compatibility between source and target */
		bool source_is_dir = ext4_inode_is_type(&fs->sb, child_ref.inode, EXT4_INODE_MODE_DIRECTORY);
		bool target_is_dir = ext4_inode_is_type(&fs->sb, existing_ref.inode, EXT4_INODE_MODE_DIRECTORY);

		if (source_is_dir && !target_is_dir) {
			/* Source is directory, target is not */
			ext4_fs_put_inode_ref(&existing_ref);
			ext4_fs_put_inode_ref(&child_ref);
			r = ENOTDIR;
			goto Finish;
		}

		if (!source_is_dir && target_is_dir) {
			/* Source is not directory, target is */
			ext4_fs_put_inode_ref(&existing_ref);
			ext4_fs_put_inode_ref(&child_ref);
			r = EISDIR;
			goto Finish;
		}

		/* If target is a directory, check if it's empty */
		if (target_is_dir) {
			bool has_children;
			r = ext4_has_children(&has_children, &existing_ref);
			if (r != EOK) {
				ext4_fs_put_inode_ref(&existing_ref);
				ext4_fs_put_inode_ref(&child_ref);
				goto Finish;
			}

			if (has_children) {
				/* Target directory is not empty */
				ext4_fs_put_inode_ref(&existing_ref);
				ext4_fs_put_inode_ref(&child_ref);
				r = ENOTEMPTY;
				goto Finish;
			}
		}

		/* Remove the existing file */
		r = ext4_inode_unlink(mp, new_parent_ref, new_name, new_name_len);
		if (r != EOK) {
			ext4_fs_put_inode_ref(&existing_ref);
			ext4_fs_put_inode_ref(&child_ref);
			goto Finish;
		}

		ext4_fs_put_inode_ref(&existing_ref);
	} else if (r != ENOENT) {
		/* Error other than "not found" */
		ext4_fs_put_inode_ref(&child_ref);
		goto Finish;
	}

	/* Create the new link */
	r = ext4_link(mp, new_parent_ref, &child_ref, new_name, new_name_len, true);
	if (r != EOK) {
		ext4_fs_put_inode_ref(&child_ref);
		goto Finish;
	}

	/* Remove the old link */
	r = ext4_dir_remove_entry(old_parent_ref, old_name, old_name_len);
	if (r != EOK) {
		/* Try to undo the new link creation, but don't fail if it doesn't work */
		ext4_dir_remove_entry(new_parent_ref, new_name, new_name_len);
		ext4_fs_put_inode_ref(&child_ref);
		goto Finish;
	}

	/* Update parent link counts if moving a directory */
	if (ext4_inode_is_type(&fs->sb, child_ref.inode, EXT4_INODE_MODE_DIRECTORY)) {
		if (old_parent_ref->index != new_parent_ref->index) {
			ext4_fs_inode_links_count_dec(old_parent_ref);
			old_parent_ref->dirty = true;
			ext4_fs_inode_links_count_inc(new_parent_ref);
			new_parent_ref->dirty = true;
		}
	}

	ext4_fs_put_inode_ref(&child_ref);

Success:
	r = EOK;

Finish:
	if (r != EOK)
		ext4_trans_abort(mp);
	else
		ext4_trans_stop(mp);

	/* Workaround: Put and re-get parent references to flush data */
	uint32_t old_parent_index = old_parent_ref->index;
	uint32_t new_parent_index = new_parent_ref->index;

	ext4_fs_put_inode_ref(old_parent_ref);
	ext4_fs_put_inode_ref(new_parent_ref);

	ext4_block_cache_write_back(fs->bdev, 0);
	ext4_block_cache_flush(fs->bdev);

	ext4_fs_get_inode_ref(fs, old_parent_index, old_parent_ref);
	ext4_fs_get_inode_ref(fs, new_parent_index, new_parent_ref);

	return r;
}

int ext4_inode_hardlink(struct ext4_mountpoint *mp,
			struct ext4_inode_ref *target_ref,
			struct ext4_inode_ref *link_parent_ref,
			const char *link_name, uint32_t link_name_len)
{
	int r;
	struct ext4_fs *fs;
	uint32_t existing_inode;
	uint8_t existing_type;

	ext4_assert(mp && target_ref && link_parent_ref && link_name);

	if (link_name_len > EXT4_DIRECTORY_FILENAME_LEN)
		return EINVAL;

	fs = &mp->fs;

	if (fs->read_only)
		return EROFS;

	/* Check if parent is a directory */
	if (!ext4_inode_is_type(&fs->sb, link_parent_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return ENOTDIR;

	/* Cannot create hardlink for directories */
	if (ext4_inode_is_type(&fs->sb, target_ref->inode, EXT4_INODE_MODE_DIRECTORY))
		return EPERM;

	ext4_block_cache_write_back(fs->bdev, 1);

	/* Start transaction */
	r = ext4_trans_start(mp);
	if (r != EOK)
		return r;

	/* Check if target name already exists */
	r = ext4_inode_find_child(link_parent_ref, link_name, link_name_len, &existing_inode, &existing_type);
	if (r == EOK) {
		/* Target exists, do nothing */
		goto Success;
	} else if (r != ENOENT) {
		/* Error other than "not found" */
		goto Finish;
	}

	/* Create the hard link */
	r = ext4_link(mp, link_parent_ref, target_ref, link_name, link_name_len, false);
	if (r != EOK)
		goto Finish;

Success:
	if (r == EOK)
		r = EOK;  /* Ensure success code */

Finish:
	if (r != EOK)
		ext4_trans_abort(mp);
	else
		ext4_trans_stop(mp);

	/* Workaround: Put and re-get inode references to flush data */
	uint32_t parent_index = link_parent_ref->index;
	uint32_t target_index = target_ref->index;

	ext4_fs_put_inode_ref(link_parent_ref);
	ext4_fs_put_inode_ref(target_ref);

	ext4_block_cache_write_back(fs->bdev, 0);
	ext4_block_cache_flush(fs->bdev);

	ext4_fs_get_inode_ref(fs, parent_index, link_parent_ref);
	ext4_fs_get_inode_ref(fs, target_index, target_ref);

	return r;
}


/**
 * @}
 */
