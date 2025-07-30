/*
 * Copyright (c) 2025 lwext4_rust project
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
 * @file  ext4_inode_ops.h
 * @brief Optimized ext4 operations that work directly with inode references
 *        instead of path strings for better performance.
 */

#ifndef EXT4_INODE_OPS_H_
#define EXT4_INODE_OPS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_fs.h>
#include <ext4.h>

#include <stdint.h>
#include <stddef.h>

/********************************MOUNT OPERATIONS****************************/

/* Note: ext4_get_mount and transaction functions are declared in ext4.h */

/********************************FILE OPERATIONS*****************************/

/**@brief   Create a file by taking a parent inode reference.
 *
 * @param   mp Mount point.
 * @param   parent_ref Parent directory inode reference.
 * @param   name File name to create.
 * @param   name_len Length of file name.
 * @param   mode File mode including type bits and permission bits (e.g., EXT4_INODE_MODE_FILE | 0644).
 * @param   child_ref Output parameter for created file's inode reference.
 *
 * @return  Standard error code.*/
int ext4_inode_create_file(struct ext4_mountpoint *mp,
			   struct ext4_inode_ref *parent_ref,
			   const char *name, uint32_t name_len,
			   uint32_t mode, struct ext4_inode_ref *child_ref);

/**@brief   Unlink a file or directory.
 *
 * @param   mp Mount point.
 * @param   parent_ref Parent directory inode reference.
 * @param   name Name of the entry to unlink.
 * @param   name_len Length of the name.
 *
 * @return  Standard error code.*/
int ext4_inode_unlink(struct ext4_mountpoint *mp,
		      struct ext4_inode_ref *parent_ref,
		      const char *name, uint32_t name_len);

/**@brief   Look for a child directory entry by name in the given inode.
 *
 * @param   parent_ref Parent directory inode reference.
 * @param   name Name of child to find.
 * @param   name_len Length of name.
 * @param   child_inode Output parameter for child inode number.
 * @param   child_type Output parameter for child type (optional, can be NULL).
 *
 * @return  Standard error code.*/
int ext4_inode_find_child(struct ext4_inode_ref *parent_ref,
			  const char *name, uint32_t name_len,
			  uint32_t *child_inode, uint8_t *child_type);

/**@brief   Open a file given the inode reference.
 *
 * @param   file File handle to initialize.
 * @param   mp Mount point.
 * @param   inode_ref Inode reference to open.
 * @param   flags Open flags.
 *
 * @return  Standard error code.*/
int ext4_inode_fopen(ext4_file *file, struct ext4_mountpoint *mp,
		     struct ext4_inode_ref *inode_ref, uint32_t flags);

/**@brief   Rename (move) a file or directory using inode references.
 *
 * @param   mp Mount point.
 * @param   old_parent_ref Parent directory inode reference of the source.
 * @param   old_name Current name of the file/directory.
 * @param   old_name_len Length of current name.
 * @param   new_parent_ref Parent directory inode reference of the destination.
 * @param   new_name New name for the file/directory.
 * @param   new_name_len Length of new name.
 *
 * @return  Standard error code.*/
int ext4_inode_rename(struct ext4_mountpoint *mp,
		      struct ext4_inode_ref *old_parent_ref,
		      const char *old_name, uint32_t old_name_len,
		      struct ext4_inode_ref *new_parent_ref,
		      const char *new_name, uint32_t new_name_len);

/**@brief   Create a hard link to a file using inode references.
 *
 * @param   mp Mount point.
 * @param   target_ref Inode reference of the file to link to.
 * @param   link_parent_ref Parent directory inode reference for the new link.
 * @param   link_name Name for the new link.
 * @param   link_name_len Length of link name.
 *
 * @return  Standard error code.*/
int ext4_inode_hardlink(struct ext4_mountpoint *mp,
			struct ext4_inode_ref *target_ref,
			struct ext4_inode_ref *link_parent_ref,
			const char *link_name, uint32_t link_name_len);

#ifdef __cplusplus
}
#endif

#endif /* EXT4_INODE_OPS_H_ */

/**
 * @}
 */
