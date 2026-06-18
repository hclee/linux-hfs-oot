/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KCOMPAT_H
#define _KCOMPAT_H

#include <linux/version.h>

/*
 * Compatibility header for older kernels.
 * Active build target is v7.1 (which requires no compatibility shims).
 */

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

#include <linux/err.h>

/*
 * 1) Purpose: Introduce fallback for kzalloc_obj() which was added in v6.13.
 * 2) Type: Fallback macro definition.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#ifndef kzalloc_obj
#define kzalloc_obj(x) kzalloc(sizeof(x), GFP_KERNEL)
#endif

/*
 * 1) Purpose: Introduce fallback for kmalloc_obj() which was added in v6.13.
 * 2) Type: Fallback macro definition.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#ifndef kmalloc_obj
#define kmalloc_obj(x) kmalloc(sizeof(x), GFP_KERNEL)
#endif

/*
 * 1) Purpose: Introduce fallback for inode_state_read_once() which was added in v6.13.
 * 2) Type: Static inline function fallback.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
static inline unsigned int inode_state_read_once(const struct inode *inode)
{
	return READ_ONCE(inode->i_state);
}
#endif

/*
 * 1) Purpose: Adapt to VFS .mkdir API signature change (returns dentry* in v7.1 vs. int in v6.12).
 * 2) Type: Parameterized macro redirection with static inline wrapper.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)

#ifdef COMPAT_COMPILING_HFS_DIR
static struct dentry *hfs_mkdir_upstream(struct mnt_idmap *idmap, struct inode *dir,
					 struct dentry *dentry, umode_t mode);

static inline int hfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, umode_t mode)
{
	struct dentry *d = hfs_mkdir_upstream(idmap, dir, dentry, mode);
	return PTR_ERR_OR_ZERO(d);
}

#define hfs_mkdir(idmap, dir, dentry, mode) \
	hfs_mkdir_upstream(idmap, dir, dentry, mode)
#else
static inline int hfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, umode_t mode)
{
	return 0;
}
#endif

#ifdef COMPAT_COMPILING_HFSPLUS_DIR
static struct dentry *hfsplus_mkdir_upstream(struct mnt_idmap *idmap, struct inode *dir,
					     struct dentry *dentry, umode_t mode);

static inline int hfsplus_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				struct dentry *dentry, umode_t mode)
{
	struct dentry *d = hfsplus_mkdir_upstream(idmap, dir, dentry, mode);
	return PTR_ERR_OR_ZERO(d);
}

#define hfsplus_mkdir(idmap, dir, dentry, mode) \
	hfsplus_mkdir_upstream(idmap, dir, dentry, mode)
#else
static inline int hfsplus_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				struct dentry *dentry, umode_t mode)
{
	return 0;
}
#endif

#endif

/*
 * 1) Purpose: Adapt to cont_write_begin() signature difference (takes iocb in v7.1 vs. file in v6.12).
 * 2) Type: Parameterized macro argument mapping.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
#define cont_write_begin(iocb, mapping, pos, len, foliop, fsdata, get_block, bytes) \
	cont_write_begin((iocb) ? (iocb)->ki_filp : NULL, mapping, pos, len, foliop, fsdata, get_block, bytes)
#endif

/*
 * 1) Purpose: Adapt to .write_begin API signature change (takes iocb in v7.1 vs. file in v6.12).
 * 2) Type: Parameterized macro redirection with static inline wrapper.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
int hfs_write_begin_upstream(const struct kiocb *iocb, struct address_space *mapping,
			     loff_t pos, unsigned int len, struct folio **foliop,
			     void **fsdata);

static inline int (hfs_write_begin)(struct file *file, struct address_space *mapping,
				    loff_t pos, unsigned int len, struct folio **foliop,
				    void **fsdata)
{
	struct kiocb iocb = {
		.ki_filp = file,
	};
	return hfs_write_begin_upstream(&iocb, mapping, pos, len, foliop, fsdata);
}

#define hfs_write_begin(iocb, mapping, pos, len, foliop, fsdata) \
	hfs_write_begin_upstream(iocb, mapping, pos, len, foliop, fsdata)

int hfsplus_write_begin_upstream(const struct kiocb *iocb, struct address_space *mapping,
				 loff_t pos, unsigned int len, struct folio **foliop,
				 void **fsdata);

static inline int (hfsplus_write_begin)(struct file *file, struct address_space *mapping,
					loff_t pos, unsigned int len, struct folio **foliop,
					void **fsdata)
{
	struct kiocb iocb = {
		.ki_filp = file,
	};
	return hfsplus_write_begin_upstream(&iocb, mapping, pos, len, foliop, fsdata);
}

#define hfsplus_write_begin(iocb, mapping, pos, len, foliop, fsdata) \
	hfsplus_write_begin_upstream(iocb, mapping, pos, len, foliop, fsdata)
#endif

/*
 * 1) Purpose: Adapt to .mmap_prepare renaming (introduced in v6.17; maps to .mmap on older kernels).
 * 2) Type: Simple macro renaming mapping.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
#define mmap_prepare mmap
#define generic_file_mmap_prepare generic_file_mmap
#endif

/*
 * 1) Purpose: Provide set_default_d_op() helper which was introduced in v6.13.
 * 2) Type: Static inline function fallback.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
static inline void set_default_d_op(struct super_block *s, const struct dentry_operations *ops)
{
	s->s_d_op = ops;
}
#endif

/*
 * 1) Purpose: Adapt to structure rename from fileattr to file_kattr in v6.14.
 * 2) Type: Simple macro renaming mapping.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
#define file_kattr fileattr
#endif

/*
 * 1) Purpose: Adapt to .d_revalidate API signature change (takes dir and name in v6.14 vs. only dentry in v6.12).
 * 2) Type: Parameterized macro redirection with static inline wrapper.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
#ifdef COMPAT_COMPILING_HFS_SYSDEP
static int hfs_revalidate_dentry_upstream(struct inode *dir, const struct qstr *name,
					  struct dentry *dentry, unsigned int flags);

static inline int (hfs_revalidate_dentry)(struct dentry *dentry, unsigned int flags)
{
	return hfs_revalidate_dentry_upstream(NULL, NULL, dentry, flags);
}

#define hfs_revalidate_dentry(dir, name, dentry, flags) \
	hfs_revalidate_dentry_upstream(dir, name, dentry, flags)
#else
static inline int (hfs_revalidate_dentry)(struct dentry *dentry, unsigned int flags)
{
	return 1;
}
#endif
#endif
#include <linux/bio.h>
#include <linux/blkdev.h>

/*
 * 1) Purpose: Implement fallback for bdev_rw_virt() which was introduced in v6.13.
 * 2) Type: Static inline function fallback.
 * 3) Target Kernels: Builds successfully on v6.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
static inline int bdev_rw_virt(struct block_device *bdev, sector_t sector, void *data,
			       size_t len, enum req_op op)
{
	struct bio_vec bv;
	struct bio bio;
	int error;

	if (WARN_ON_ONCE(is_vmalloc_addr(data)))
		return -EIO;

	bio_init(&bio, bdev, &bv, 1, op);
	bio.bi_iter.bi_sector = sector;
	if (bio_add_page(&bio, virt_to_page(data), len, offset_in_page(data)) != len) {
		bio_uninit(&bio);
		return -EIO;
	}
	error = submit_bio_wait(&bio);
	bio_uninit(&bio);
	return error;
}
#endif

#endif /* _KCOMPAT_H */
