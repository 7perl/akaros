/* Copyright (c) 2009, 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Default implementations and global values for the VFS. */

#include <vfs.h> // keep this first
#include <sys/queue.h>
#include <assert.h>
#include <stdio.h>
#include <atomic.h>
#include <slab.h>
#include <kmalloc.h>
#include <kfs.h>
#include <ext2fs.h>
#include <pmap.h>
#include <umem.h>
#include <smp.h>

struct sb_tailq super_blocks = TAILQ_HEAD_INITIALIZER(super_blocks);
spinlock_t super_blocks_lock = SPINLOCK_INITIALIZER;
struct fs_type_tailq file_systems = TAILQ_HEAD_INITIALIZER(file_systems);
struct namespace default_ns;

struct kmem_cache *dentry_kcache; // not to be confused with the dcache
struct kmem_cache *inode_kcache;
struct kmem_cache *file_kcache;

/* Mounts fs from dev_name at mnt_pt in namespace ns.  There could be no mnt_pt,
 * such as with the root of (the default) namespace.  Not sure how it would work
 * with multiple namespaces on the same FS yet.  Note if you mount the same FS
 * multiple times, you only have one FS still (and one SB).  If we ever support
 * that... */
struct vfsmount *__mount_fs(struct fs_type *fs, char *dev_name,
                            struct dentry *mnt_pt, int flags,
                            struct namespace *ns)
{
	struct super_block *sb;
	struct vfsmount *vmnt = kmalloc(sizeof(struct vfsmount), 0);

	/* this first ref is stored in the NS tailq below */
	kref_init(&vmnt->mnt_kref, fake_release, 1);
	/* Build the vfsmount, if there is no mnt_pt, mnt is the root vfsmount (for
	 * now).  fields related to the actual FS, like the sb and the mnt_root are
	 * set in the fs-specific get_sb() call. */
	if (!mnt_pt) {
		vmnt->mnt_parent = NULL;
		vmnt->mnt_mountpoint = NULL;
	} else { /* common case, but won't be tested til we try to mount another FS */
		mnt_pt->d_mount_point = TRUE;
		mnt_pt->d_mounted_fs = vmnt;
		kref_get(&vmnt->mnt_kref, 1); /* held by mnt_pt */
		vmnt->mnt_parent = mnt_pt->d_sb->s_mount;
		vmnt->mnt_mountpoint = mnt_pt;
	}
	TAILQ_INIT(&vmnt->mnt_child_mounts);
	vmnt->mnt_flags = flags;
	vmnt->mnt_devname = dev_name;
	vmnt->mnt_namespace = ns;
	kref_get(&ns->kref, 1); /* held by vmnt */

	/* Read in / create the SB */
	sb = fs->get_sb(fs, flags, dev_name, vmnt);
	if (!sb)
		panic("You're FS sucks");

	/* TODO: consider moving this into get_sb or something, in case the SB
	 * already exists (mounting again) (if we support that) */
	spin_lock(&super_blocks_lock);
	TAILQ_INSERT_TAIL(&super_blocks, sb, s_list); /* storing a ref here... */
	spin_unlock(&super_blocks_lock);

	/* Update holding NS */
	spin_lock(&ns->lock);
	TAILQ_INSERT_TAIL(&ns->vfsmounts, vmnt, mnt_list);
	spin_unlock(&ns->lock);
	/* note to self: so, right after this point, the NS points to the root FS
	 * mount (we return the mnt, which gets assigned), the root mnt has a dentry
	 * for /, backed by an inode, with a SB prepped and in memory. */
	return vmnt;
}

void vfs_init(void)
{
	struct fs_type *fs;

	dentry_kcache = kmem_cache_create("dentry", sizeof(struct dentry),
	                                  __alignof__(struct dentry), 0, 0, 0);
	inode_kcache = kmem_cache_create("inode", sizeof(struct inode),
	                                 __alignof__(struct inode), 0, 0, 0);
	file_kcache = kmem_cache_create("file", sizeof(struct file),
	                                __alignof__(struct file), 0, 0, 0);
	/* default NS never dies, +1 to exist */
	kref_init(&default_ns.kref, fake_release, 1);
	spinlock_init(&default_ns.lock);
	default_ns.root = NULL;
	TAILQ_INIT(&default_ns.vfsmounts);

	/* build list of all FS's in the system.  put yours here.  if this is ever
	 * done on the fly, we'll need to lock. */
	TAILQ_INSERT_TAIL(&file_systems, &kfs_fs_type, list);
#ifdef CONFIG_EXT2FS
	TAILQ_INSERT_TAIL(&file_systems, &ext2_fs_type, list);
#endif
	TAILQ_FOREACH(fs, &file_systems, list)
		printk("Supports the %s Filesystem\n", fs->name);

	/* mounting KFS at the root (/), pending root= parameters */
	// TODO: linux creates a temp root_fs, then mounts the real root onto that
	default_ns.root = __mount_fs(&kfs_fs_type, "RAM", NULL, 0, &default_ns);

	printk("vfs_init() completed\n");
}

/* FS's can provide another, if they want */
int generic_dentry_hash(struct dentry *dentry, struct qstr *qstr)
{
	unsigned long hash = 5381;

	for (int i = 0; i < qstr->len; i++) {
		/* hash * 33 + c, djb2's technique */
		hash = ((hash << 5) + hash) + qstr->name[i];
	}
	return hash;
}

/* Builds / populates the qstr of a dentry based on its d_iname.  If there is an
 * l_name, (long), it will use that instead of the inline name.  This will
 * probably change a bit. */
void qstr_builder(struct dentry *dentry, char *l_name)
{
	dentry->d_name.name = l_name ? l_name : dentry->d_iname;
	dentry->d_name.len = strnlen(dentry->d_name.name, MAX_FILENAME_SZ);
	dentry->d_name.hash = dentry->d_op->d_hash(dentry, &dentry->d_name);
}

/* Useful little helper - return the string ptr for a given file */
char *file_name(struct file *file)
{
	return file->f_dentry->d_name.name;
}

/* Some issues with this, coupled closely to fs_lookup.
 *
 * Note the use of __dentry_free, instead of kref_put.  In those cases, we don't
 * want to treat it like a kref and we have the only reference to it, so it is
 * okay to do this.  It makes dentry_release() easier too. */
static struct dentry *do_lookup(struct dentry *parent, char *name)
{
	struct dentry *result, *query;
	query = get_dentry(parent->d_sb, parent, name);
	if (!query) {
		warn("OOM in do_lookup(), probably wasn't expected\n");
		return 0;
	}
	result = dcache_get(parent->d_sb, query); 
	if (result) {
		__dentry_free(query);
		return result;
	}
	/* No result, check for negative */
	if (query->d_flags & DENTRY_NEGATIVE) {
		__dentry_free(query);
		return 0;
	}
	/* not in the dcache at all, need to consult the FS */
	result = parent->d_inode->i_op->lookup(parent->d_inode, query, 0);
	if (!result) {
		/* Note the USED flag will get turned off when this gets added to the
		 * LRU in dentry_release().  There's a slight race here that we'll panic
		 * on, but I want to catch it (in dcache_put()) for now. */
		query->d_flags |= DENTRY_NEGATIVE;
		dcache_put(parent->d_sb, query);
		kref_put(&query->d_kref);
		return 0;
	}
	dcache_put(parent->d_sb, result);
	/* This is because KFS doesn't return the same dentry, but ext2 does.  this
	 * is ugly and needs to be fixed. (TODO) */
	if (result != query)
		__dentry_free(query);

	/* TODO: if the following are done by us, how do we know the i_ino?
	 * also need to handle inodes that are already read in!  For now, we're
	 * going to have the FS handle it in it's lookup() method: 
	 * - get a new inode
	 * - read in the inode
	 * - put in the inode cache */
	return result;
}

/* Update ND such that it represents having followed dentry.  IAW the nd
 * refcnting rules, we need to decref any references that were in there before
 * they get clobbered. */
static int next_link(struct dentry *dentry, struct nameidata *nd)
{
	assert(nd->dentry && nd->mnt);
	/* update the dentry */
	kref_get(&dentry->d_kref, 1);
	kref_put(&nd->dentry->d_kref);
   	nd->dentry = dentry;
	/* update the mount, if we need to */
	if (dentry->d_sb->s_mount != nd->mnt) {
		kref_get(&dentry->d_sb->s_mount->mnt_kref, 1);
		kref_put(&nd->mnt->mnt_kref);
   		nd->mnt = dentry->d_sb->s_mount;
	}
	return 0;
}

/* Walk up one directory, being careful of mountpoints, namespaces, and the top
 * of the FS */
static int climb_up(struct nameidata *nd)
{
	printd("CLIMB_UP, from %s\n", nd->dentry->d_name.name);
	/* Top of the world, just return.  Should also check for being at the top of
	 * the current process's namespace (TODO) */
	if (!nd->dentry->d_parent || (nd->dentry->d_parent == nd->dentry))
		return -1;
	/* Check if we are at the top of a mount, if so, we need to follow
	 * backwards, and then climb_up from that one.  We might need to climb
	 * multiple times if we mount multiple FSs at the same spot (highly
	 * unlikely).  This is completely untested.  Might recurse instead. */
	while (nd->mnt->mnt_root == nd->dentry) {
		if (!nd->mnt->mnt_parent) {
			warn("Might have expected a parent vfsmount (dentry had a parent)");
			return -1;
		}
		next_link(nd->mnt->mnt_mountpoint, nd);
	}
	/* Backwards walk (no mounts or any other issues now). */
	next_link(nd->dentry->d_parent, nd);
	printd("CLIMB_UP, to   %s\n", nd->dentry->d_name.name);
	return 0;
}

/* nd->dentry might be on a mount point, so we need to move on to the child
 * mount's root. */
static int follow_mount(struct nameidata *nd)
{
	if (!nd->dentry->d_mount_point)
		return 0;
	next_link(nd->dentry->d_mounted_fs->mnt_root, nd);
	return 0;
}

static int link_path_walk(char *path, struct nameidata *nd);

/* When nd->dentry is for a symlink, this will recurse and follow that symlink,
 * so that nd contains the results of following the symlink (dentry and mnt).
 * Returns when it isn't a symlink, 1 on following a link, and < 0 on error. */
static int follow_symlink(struct nameidata *nd)
{
	int retval;
	char *symname;
	if (!S_ISLNK(nd->dentry->d_inode->i_mode))
		return 0;
	if (nd->depth > MAX_SYMLINK_DEPTH)
		return -ELOOP;
	printd("Following symlink for dentry %p %s\n", nd->dentry,
	       nd->dentry->d_name.name);
	nd->depth++;
	symname = nd->dentry->d_inode->i_op->readlink(nd->dentry);
	/* We need to pin in nd->dentry (the dentry of the symlink), since we need
	 * it's symname's storage to stay in memory throughout the upcoming
	 * link_path_walk().  The last_sym gets decreffed when we path_release() or
	 * follow another symlink. */
	if (nd->last_sym)
		kref_put(&nd->last_sym->d_kref);
	kref_get(&nd->dentry->d_kref, 1);
	nd->last_sym = nd->dentry;
	/* If this an absolute path in the symlink, we need to free the old path and
	 * start over, otherwise, we continue from the PARENT of nd (the symlink) */
	if (symname[0] == '/') {
		path_release(nd);
		if (!current)
			nd->dentry = default_ns.root->mnt_root;
		else
			nd->dentry = current->fs_env.root;	
		nd->mnt = nd->dentry->d_sb->s_mount;
		kref_get(&nd->mnt->mnt_kref, 1);
		kref_get(&nd->dentry->d_kref, 1);
	} else {
		climb_up(nd);
	}
	/* either way, keep on walking in the free world! */
	retval = link_path_walk(symname, nd);
	return (retval == 0 ? 1 : retval);
}

/* Little helper, to make it easier to break out of the nested loops.  Will also
 * '\0' out the first slash if it's slashes all the way down.  Or turtles. */
static bool packed_trailing_slashes(char *first_slash)
{
	for (char *i = first_slash; *i == '/'; i++) {
		if (*(i + 1) == '\0') {
			*first_slash = '\0';
			return TRUE;
		}
	}
	return FALSE;
}

/* Simple helper to set nd to track it's last name to be Name.  Also be careful
 * with the storage of name.  Don't use and nd's name past the lifetime of the
 * string used in the path_lookup()/link_path_walk/whatever.  Consider replacing
 * parts of this with a qstr builder.  Note this uses the dentry's d_op, which
 * might not be the dentry we care about. */
static void stash_nd_name(struct nameidata *nd, char *name)
{
	nd->last.name = name;
	nd->last.len = strlen(name);
	nd->last.hash = nd->dentry->d_op->d_hash(nd->dentry, &nd->last);
}

/* Resolves the links in a basic path walk.  0 for success, -EWHATEVER
 * otherwise.  The final lookup is returned via nd. */
static int link_path_walk(char *path, struct nameidata *nd)
{
	struct dentry *link_dentry;
	struct inode *link_inode, *nd_inode;
	char *next_slash;
	char *link = path;
	int error;

	/* Prevent crazy recursion */
	if (nd->depth > MAX_SYMLINK_DEPTH)
		return -ELOOP;
	/* skip all leading /'s */
	while (*link == '/')
		link++;
	/* if there's nothing left (null terminated), we're done.  This should only
	 * happen for "/", which if we wanted a PARENT, should fail (there is no
	 * parent). */
	if (*link == '\0') {
		if (nd->flags & LOOKUP_PARENT) {
			set_errno(ENOENT);
			return -1;
		}
		/* o/w, we're good */
		return 0;
	}
	/* iterate through each intermediate link of the path.  in general, nd
	 * tracks where we are in the path, as far as dentries go.  once we have the
	 * next dentry, we try to update nd based on that dentry.  link is the part
	 * of the path string that we are looking up */
	while (1) {
		nd_inode = nd->dentry->d_inode;
		if ((error = check_perms(nd_inode, nd->intent)))
			return error;
		/* find the next link, break out if it is the end */
		next_slash = strchr(link, '/');
		if (!next_slash) {
			break;
		} else {
			if (packed_trailing_slashes(next_slash)) {
				nd->flags |= LOOKUP_DIRECTORY;
				break;
			}
		}
		/* skip over any interim ./ */
		if (!strncmp("./", link, 2))
			goto next_loop;
		/* Check for "../", walk up */
		if (!strncmp("../", link, 3)) {
			climb_up(nd);
			goto next_loop;
		}
		*next_slash = '\0';
		link_dentry = do_lookup(nd->dentry, link);
		*next_slash = '/';
		if (!link_dentry)
			return -ENOENT;
		/* make link_dentry the current step/answer */
		next_link(link_dentry, nd);
		kref_put(&link_dentry->d_kref);	/* do_lookup gave us a refcnt dentry */
		/* we could be on a mountpoint or a symlink - need to follow them */
		follow_mount(nd);
		if ((error = follow_symlink(nd)) < 0)
			return error;
		/* Turn off a possible DIRECTORY lookup, which could have been set
		 * during the follow_symlink (a symlink could have had a directory at
		 * the end), though it was in the middle of the real path. */
		nd->flags &= ~LOOKUP_DIRECTORY;
		if (!S_ISDIR(nd->dentry->d_inode->i_mode))
			return -ENOTDIR;
next_loop:
		/* move through the path string to the next entry */
		link = next_slash + 1;
		/* advance past any other interim slashes.  we know we won't hit the end
		 * due to the for loop check above */
		while (*link == '/')
			link++;
	}
	/* Now, we're on the last link of the path.  We need to deal with with . and
	 * .. .  This might be weird with PARENT lookups - not sure what semantics
	 * we want exactly.  This will give the parent of whatever the PATH was
	 * supposed to look like.  Note that ND currently points to the parent of
	 * the last item (link). */
	if (!strcmp(".", link)) {
		if (nd->flags & LOOKUP_PARENT) {
			assert(nd->dentry->d_name.name);
			stash_nd_name(nd, nd->dentry->d_name.name);
			climb_up(nd);
		}
		return 0;
	}
	if (!strcmp("..", link)) {
		climb_up(nd);
		if (nd->flags & LOOKUP_PARENT) {
			assert(nd->dentry->d_name.name);
			stash_nd_name(nd, nd->dentry->d_name.name);
			climb_up(nd);
		}
		return 0;
	}
	/* need to attempt to look it up, in case it's a symlink */
	link_dentry = do_lookup(nd->dentry, link);
	if (!link_dentry) {
		/* if there's no dentry, we are okay if we are looking for the parent */
		if (nd->flags & LOOKUP_PARENT) {
			assert(strcmp(link, ""));
			stash_nd_name(nd, link);
			return 0;
		} else {
			return -ENOENT;
		}
	}
	next_link(link_dentry, nd);
	kref_put(&link_dentry->d_kref);	/* do_lookup gave us a refcnt'd dentry */
	/* at this point, nd is on the final link, but it might be a symlink */
	if (nd->flags & LOOKUP_FOLLOW) {
		error = follow_symlink(nd);
		if (error < 0)
			return error;
		/* if we actually followed a symlink, then nd is set and we're done */
		if (error > 0)
			return 0;
	}
	/* One way or another, nd is on the last element of the path, symlinks and
	 * all.  Now we need to climb up to set nd back on the parent, if that's
	 * what we wanted */
	if (nd->flags & LOOKUP_PARENT) {
		assert(nd->dentry->d_name.name);
		stash_nd_name(nd, link_dentry->d_name.name);
		climb_up(nd);
		return 0;
	}
	/* now, we have the dentry set, and don't want the parent, but might be on a
	 * mountpoint still.  FYI: this hasn't been thought through completely. */
	follow_mount(nd);
	/* If we wanted a directory, but didn't get one, error out */
	if ((nd->flags & LOOKUP_DIRECTORY) && !S_ISDIR(nd->dentry->d_inode->i_mode))
		return -ENOTDIR;
	return 0;
}

/* Given path, return the inode for the final dentry.  The ND should be
 * initialized for the first call - specifically, we need the intent. 
 * LOOKUP_PARENT and friends go in the flags var, which is not the intent.
 *
 * If path_lookup wants a PARENT, but hits the top of the FS (root or
 * otherwise), we want it to error out.  It's still unclear how we want to
 * handle processes with roots that aren't root, but at the very least, we don't
 * want to think we have the parent of /, but have / itself.  Due to the way
 * link_path_walk works, if that happened, we probably don't have a
 * nd->last.name.  This needs more thought (TODO).
 *
 * Need to be careful too.  While the path has been copied-in to the kernel,
 * it's still user input.  */
int path_lookup(char *path, int flags, struct nameidata *nd)
{
	int retval;
	printd("Path lookup for %s\n", path);
	/* we allow absolute lookups with no process context */
	/* TODO: RCU read lock on pwd or kref_not_zero in a loop.  concurrent chdir
	 * could decref nd->dentry before we get to incref it below. */
	if (path[0] == '/') {			/* absolute lookup */
		if (!current)
			nd->dentry = default_ns.root->mnt_root;
		else
			nd->dentry = current->fs_env.root;	
	} else {						/* relative lookup */
		assert(current);
		/* Don't need to lock on the fs_env since we're reading one item */
		nd->dentry = current->fs_env.pwd;	
	}
	nd->mnt = nd->dentry->d_sb->s_mount;
	/* Whenever references get put in the nd, incref them.  Whenever they are
	 * removed, decref them. */
	kref_get(&nd->mnt->mnt_kref, 1);
	kref_get(&nd->dentry->d_kref, 1);
	nd->flags = flags;
	nd->depth = 0;					/* used in symlink following */
	retval =  link_path_walk(path, nd);	
	/* make sure our PARENT lookup worked */
	if (!retval && (flags & LOOKUP_PARENT))
		assert(nd->last.name);
	return retval;
}

/* Call this after any use of path_lookup when you are done with its results,
 * regardless of whether it succeeded or not.  It will free any references */
void path_release(struct nameidata *nd)
{
	kref_put(&nd->dentry->d_kref);
	kref_put(&nd->mnt->mnt_kref);
	/* Free the last symlink dentry used, if there was one */
	if (nd->last_sym) {
		kref_put(&nd->last_sym->d_kref);
		nd->last_sym = 0;			/* catch reuse bugs */
	}
}

/* External version of mount, only call this after having a / mount */
int mount_fs(struct fs_type *fs, char *dev_name, char *path, int flags)
{
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int retval = 0;
	retval = path_lookup(path, LOOKUP_DIRECTORY, nd);
	if (retval)
		goto out;
	/* taking the namespace of the vfsmount of path */ 
	if (!__mount_fs(fs, dev_name, nd->dentry, flags, nd->mnt->mnt_namespace))
		retval = -EINVAL;
out:
	path_release(nd);
	return retval;
}

/* Superblock functions */

/* Dentry "hash" function for the hash table to use.  Since we already have the
 * hash in the qstr, we don't need to rehash.  Also, note we'll be using the
 * dentry in question as both the key and the value. */
static size_t __dcache_hash(void *k)
{
	return (size_t)((struct dentry*)k)->d_name.hash;
}

/* Dentry cache hashtable equality function.  This means we need to pass in some
 * minimal dentry when doing a lookup. */
static ssize_t __dcache_eq(void *k1, void *k2)
{
	if (((struct dentry*)k1)->d_parent != ((struct dentry*)k2)->d_parent)
		return 0;
	/* TODO: use the FS-specific string comparison */
	return !strcmp(((struct dentry*)k1)->d_name.name,
	               ((struct dentry*)k2)->d_name.name);
}

/* Helper to alloc and initialize a generic superblock.  This handles all the
 * VFS related things, like lists.  Each FS will need to handle its own things
 * in it's *_get_sb(), usually involving reading off the disc. */
struct super_block *get_sb(void)
{
	struct super_block *sb = kmalloc(sizeof(struct super_block), 0);
	sb->s_dirty = FALSE;
	spinlock_init(&sb->s_lock);
	kref_init(&sb->s_kref, fake_release, 1); /* for the ref passed out */
	TAILQ_INIT(&sb->s_inodes);
	TAILQ_INIT(&sb->s_dirty_i);
	TAILQ_INIT(&sb->s_io_wb);
	TAILQ_INIT(&sb->s_lru_d);
	TAILQ_INIT(&sb->s_files);
	sb->s_dcache = create_hashtable(100, __dcache_hash, __dcache_eq);
	sb->s_icache = create_hashtable(100, __generic_hash, __generic_eq);
	spinlock_init(&sb->s_lru_lock);
	spinlock_init(&sb->s_dcache_lock);
	spinlock_init(&sb->s_icache_lock);
	sb->s_fs_info = 0; // can override somewhere else
	return sb;
}

/* Final stages of initializing a super block, including creating and linking
 * the root dentry, root inode, vmnt, and sb.  The d_op and root_ino are
 * FS-specific, but otherwise it's FS-independent, tricky, and not worth having
 * around multiple times.
 *
 * Not the world's best interface, so it's subject to change, esp since we're
 * passing (now 3) FS-specific things. */
void init_sb(struct super_block *sb, struct vfsmount *vmnt,
             struct dentry_operations *d_op, unsigned long root_ino,
             void *d_fs_info)
{
	/* Build and init the first dentry / inode.  The dentry ref is stored later
	 * by vfsmount's mnt_root.  The parent is dealt with later. */
	struct dentry *d_root = get_dentry_with_ops(sb, 0,  "/", d_op);

	if (!d_root)
		panic("OOM!  init_sb() can't fail yet!");
	/* a lot of here on down is normally done in lookup() or create, since
	 * get_dentry isn't a fully usable dentry.  The two FS-specific settings are
	 * normally inherited from a parent within the same FS in get_dentry, but we
	 * have none here. */
	d_root->d_op = d_op;
	d_root->d_fs_info = d_fs_info;
	struct inode *inode = get_inode(d_root);
	if (!inode)
		panic("This FS sucks!");
	inode->i_ino = root_ino;
	/* TODO: add the inode to the appropriate list (off i_list) */
	/* TODO: do we need to read in the inode?  can we do this on demand? */
	/* if this FS is already mounted, we'll need to do something different. */
	sb->s_op->read_inode(inode);
	icache_put(sb, inode);
	/* Link the dentry and SB to the VFS mount */
	vmnt->mnt_root = d_root;				/* ref comes from get_dentry */
	vmnt->mnt_sb = sb;
	/* If there is no mount point, there is no parent.  This is true only for
	 * the rootfs. */
	if (vmnt->mnt_mountpoint) {
		kref_get(&vmnt->mnt_mountpoint->d_kref, 1);	/* held by d_root */
		d_root->d_parent = vmnt->mnt_mountpoint;	/* dentry of the root */
	} else {
		d_root->d_parent = d_root;			/* set root as its own parent */
	}
	/* insert the dentry into the dentry cache.  when's the earliest we can?
	 * when's the earliest we should?  what about concurrent accesses to the
	 * same dentry?  should be locking the dentry... */
	dcache_put(sb, d_root);
	kref_put(&inode->i_kref);		/* give up the ref from get_inode() */
}

/* Dentry Functions */

static void dentry_set_name(struct dentry *dentry, char *name)
{
	size_t name_len = strnlen(name, MAX_FILENAME_SZ);	/* not including \0! */
	char *l_name = 0;
	if (name_len < DNAME_INLINE_LEN) {
		strncpy(dentry->d_iname, name, name_len);
		dentry->d_iname[name_len] = '\0';
		qstr_builder(dentry, 0);
	} else {
		l_name = kmalloc(name_len + 1, 0);
		assert(l_name);
		strncpy(l_name, name, name_len);
		l_name[name_len] = '\0';
		qstr_builder(dentry, l_name);
	}
}

/* Gets a dentry.  If there is no parent, use d_op.  Only called directly by
 * superblock init code. */
struct dentry *get_dentry_with_ops(struct super_block *sb,
                                   struct dentry *parent, char *name,
                                   struct dentry_operations *d_op)
{
	assert(name);
	struct dentry *dentry = kmem_cache_alloc(dentry_kcache, 0);

	if (!dentry) {
		set_errno(ENOMEM);
		return 0;
	}
	//memset(dentry, 0, sizeof(struct dentry));
	kref_init(&dentry->d_kref, dentry_release, 1);	/* this ref is returned */
	spinlock_init(&dentry->d_lock);
	TAILQ_INIT(&dentry->d_subdirs);
	dentry->d_time = 0;
	kref_get(&sb->s_kref, 1);
	dentry->d_sb = sb;					/* storing a ref here... */
	dentry->d_mount_point = FALSE;
	dentry->d_mounted_fs = 0;
	if (parent)	{						/* no parent for rootfs mount */
		kref_get(&parent->d_kref, 1);
		dentry->d_op = parent->d_op;	/* d_op set in init_sb for parentless */
	} else {
		dentry->d_op = d_op;
	}
	dentry->d_parent = parent;
	dentry->d_flags = DENTRY_USED;
	dentry->d_fs_info = 0;
	dentry_set_name(dentry, name);
	/* Catch bugs by aggressively zeroing this (o/w we use old stuff) */
	dentry->d_inode = 0;
	return dentry;
}

/* Helper to alloc and initialize a generic dentry.  The following needs to be
 * set still: d_op (if no parent), d_fs_info (opt), d_inode, connect the inode
 * to the dentry (and up the d_kref again), maybe dcache_put().  The inode
 * stitching is done in get_inode() or lookup (depending on the FS).
 * The setting of the d_op might be problematic when dealing with mounts.  Just
 * overwrite it.
 *
 * If the name is longer than the inline name, it will kmalloc a buffer, so
 * don't worry about the storage for *name after calling this. */
struct dentry *get_dentry(struct super_block *sb, struct dentry *parent,
                          char *name)
{
	return get_dentry_with_ops(sb, parent, name, 0);
}

/* Called when the dentry is unreferenced (after kref == 0).  This works closely
 * with the resurrection in dcache_get().
 *
 * The dentry is still in the dcache, but needs to be un-USED and added to the
 * LRU dentry list.  Even dentries that were used in a failed lookup need to be
 * cached - they ought to be the negative dentries.  Note that all dentries have
 * parents, even negative ones (it is needed to find it in the dcache). */
void dentry_release(struct kref *kref)
{
	struct dentry *dentry = container_of(kref, struct dentry, d_kref);

	printd("'Releasing' dentry %p: %s\n", dentry, dentry->d_name.name);
	/* DYING dentries (recently unlinked / rmdir'd) just get freed */
	if (dentry->d_flags & DENTRY_DYING) {
		__dentry_free(dentry);
		return;
	}
	/* This lock ensures the USED state and the TAILQ membership is in sync.
	 * Also used to check the refcnt, though that might not be necessary. */
	spin_lock(&dentry->d_lock);
	/* While locked, we need to double check the kref, in case someone already
	 * reup'd it.  Re-up? you're crazy!  Reee-up, you're outta yo mind! */
	if (!kref_refcnt(&dentry->d_kref)) {
		/* Note this is where negative dentries get set UNUSED */
		if (dentry->d_flags & DENTRY_USED) {
			dentry->d_flags &= ~DENTRY_USED;
			spin_lock(&dentry->d_sb->s_lru_lock);
			TAILQ_INSERT_TAIL(&dentry->d_sb->s_lru_d, dentry, d_lru);
			spin_unlock(&dentry->d_sb->s_lru_lock);
		} else {
			/* and make sure it wasn't USED, then UNUSED again */
			/* TODO: think about issues with this */
			warn("This should be rare.  Tell brho this happened.");
		}
	}
	spin_unlock(&dentry->d_lock);
}

/* Called when we really dealloc and get rid of a dentry (like when it is
 * removed from the dcache, either for memory or correctness reasons)
 *
 * This has to handle two types of dentries: full ones (ones that had been used)
 * and ones that had been just for lookups - hence the check for d_inode.
 *
 * Note that dentries pin and kref their inodes.  When all the dentries are
 * gone, we want the inode to be released via kref.  The inode has internal /
 * weak references to the dentry, which are not refcounted. */
void __dentry_free(struct dentry *dentry)
{
	if (dentry->d_inode)
		printd("Freeing dentry %p: %s\n", dentry, dentry->d_name.name);
	assert(dentry->d_op);	/* catch bugs.  a while back, some lacked d_op */
	dentry->d_op->d_release(dentry);
	/* TODO: check/test the boundaries on this. */
	if (dentry->d_name.len > DNAME_INLINE_LEN)
		kfree((void*)dentry->d_name.name);
	kref_put(&dentry->d_sb->s_kref);
	if (dentry->d_parent)
		kref_put(&dentry->d_parent->d_kref);
	if (dentry->d_mounted_fs)
		kref_put(&dentry->d_mounted_fs->mnt_kref);
	if (dentry->d_inode) {
		TAILQ_REMOVE(&dentry->d_inode->i_dentry, dentry, d_alias);
		kref_put(&dentry->d_inode->i_kref);	/* dentries kref inodes */
	}
	kmem_cache_free(dentry_kcache, dentry);
}

/* Looks up the dentry for the given path, returning a refcnt'd dentry (or 0).
 * Permissions are applied for the current user, which is quite a broken system
 * at the moment.  Flags are lookup flags. */
struct dentry *lookup_dentry(char *path, int flags)
{
	struct dentry *dentry;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;

	error = path_lookup(path, flags, nd);
	if (error) {
		path_release(nd);
		set_errno(-error);
		return 0;
	}
	dentry = nd->dentry;
	kref_get(&dentry->d_kref, 1);
	path_release(nd);
	return dentry;
}

/* Get a dentry from the dcache.  At a minimum, we need the name hash and parent
 * in what_i_want, though most uses will probably be from a get_dentry() call.
 * We pass in the SB in the off chance that we don't want to use a get'd dentry.
 *
 * The unusual variable name (instead of just "key" or something) is named after
 * ex-SPC Castro's porn folder.  Caller deals with the memory for what_i_want.
 *
 * If the dentry is negative, we don't return the actual result - instead, we
 * set the negative flag in 'what i want'.  The reason is we don't want to
 * kref_get() and then immediately put (causing dentry_release()).  This also
 * means that dentry_release() should never get someone who wasn't USED (barring
 * the race, which it handles).  And we don't need to ever have a dentry set as
 * USED and NEGATIVE (which is always wrong, but would be needed for a cleaner
 * dentry_release()).
 *
 * This is where we do the "kref resurrection" - we are returning a kref'd
 * object, even if it wasn't kref'd before.  This means the dcache does NOT hold
 * krefs (it is a weak/internal ref), but it is a source of kref generation.  We
 * sync up with the possible freeing of the dentry by locking the table.  See
 * Doc/kref for more info. */
struct dentry *dcache_get(struct super_block *sb, struct dentry *what_i_want)
{
	struct dentry *found;
	/* This lock protects the hash, as well as ensures the returned object
	 * doesn't get deleted/freed out from under us */
	spin_lock(&sb->s_dcache_lock);
	found = hashtable_search(sb->s_dcache, what_i_want);
	if (found) {
		if (found->d_flags & DENTRY_NEGATIVE) {
			what_i_want->d_flags |= DENTRY_NEGATIVE;
			spin_unlock(&sb->s_dcache_lock);
			return 0;
		}
		spin_lock(&found->d_lock);
		__kref_get(&found->d_kref, 1);	/* prob could be done outside the lock*/
		/* If we're here (after kreffing) and it is not USED, we are the one who
		 * should resurrect */
		if (!(found->d_flags & DENTRY_USED)) {
			found->d_flags |= DENTRY_USED;
			spin_lock(&sb->s_lru_lock);
			TAILQ_REMOVE(&sb->s_lru_d, found, d_lru);
			spin_unlock(&sb->s_lru_lock);
		}
		spin_unlock(&found->d_lock);
	}
	spin_unlock(&sb->s_dcache_lock);
	return found;
}

/* Adds a dentry to the dcache.  Note the *dentry is both the key and the value.
 * If the value was already in there (which can happen iff it was negative), for
 * now we'll remove it and put the new one in there. */
void dcache_put(struct super_block *sb, struct dentry *key_val)
{
	struct dentry *old;
	int retval;
	spin_lock(&sb->s_dcache_lock);
	old = hashtable_remove(sb->s_dcache, key_val);
	/* if it is old and non-negative, our caller lost a race with someone else
	 * adding the dentry.  but since we yanked it out, like a bunch of idiots,
	 * we still have to put it back.  should be fairly rare. */
	if (old && (old->d_flags & DENTRY_NEGATIVE)) {
		/* This is possible, but rare for now (about to be put on the LRU) */
		assert(!(old->d_flags & DENTRY_USED));
		assert(!kref_refcnt(&old->d_kref));
		spin_lock(&sb->s_lru_lock);
		TAILQ_REMOVE(&sb->s_lru_d, old, d_lru);
		spin_unlock(&sb->s_lru_lock);
		/* TODO: this seems suspect.  isn't this the same memory as key_val?
		 * in which case, we just adjust the flags (remove NEG) and reinsert? */
		assert(old != key_val); // checking TODO comment
		__dentry_free(old);
	}
	/* this returns 0 on failure (TODO: Fix this ghetto shit) */
 	retval = hashtable_insert(sb->s_dcache, key_val, key_val);
	assert(retval);
	spin_unlock(&sb->s_dcache_lock);
}

/* Will remove and return the dentry.  Caller deallocs the key, but the retval
 * won't have a reference.  * Returns 0 if it wasn't found.  Callers can't
 * assume much - they should not use the reference they *get back*, (if they
 * already had one for key, they can use that).  There may be other users out
 * there. */
struct dentry *dcache_remove(struct super_block *sb, struct dentry *key)
{
	struct dentry *retval;
	spin_lock(&sb->s_dcache_lock);
	retval = hashtable_remove(sb->s_dcache, key);
	spin_unlock(&sb->s_dcache_lock);
	return retval;
}

/* This will clean out the LRU list, which are the unused dentries of the dentry
 * cache.  This will optionally only free the negative ones.  Note that we grab
 * the hash lock for the time we traverse the LRU list - this prevents someone
 * from getting a kref from the dcache, which could cause us trouble (we rip
 * someone off the list, who isn't unused, and they try to rip them off the
 * list). */
void dcache_prune(struct super_block *sb, bool negative_only)
{
	struct dentry *d_i, *temp;
	struct dentry_tailq victims = TAILQ_HEAD_INITIALIZER(victims);

	spin_lock(&sb->s_dcache_lock);
	spin_lock(&sb->s_lru_lock);
	TAILQ_FOREACH_SAFE(d_i, &sb->s_lru_d, d_lru, temp) {
		if (!(d_i->d_flags & DENTRY_USED)) {
			if (negative_only && !(d_i->d_flags & DENTRY_NEGATIVE))
				continue;
			/* another place where we'd be better off with tools, not sol'ns */
			hashtable_remove(sb->s_dcache, d_i);
			TAILQ_REMOVE(&sb->s_lru_d, d_i, d_lru);
			TAILQ_INSERT_HEAD(&victims, d_i, d_lru);
		}
	}
	spin_unlock(&sb->s_lru_lock);
	spin_unlock(&sb->s_dcache_lock);
	/* Now do the actual freeing, outside of the hash/LRU list locks.  This is
	 * necessary since __dentry_free() will decref its parent, which may get
	 * released and try to add itself to the LRU. */
	TAILQ_FOREACH_SAFE(d_i, &victims, d_lru, temp) {
		TAILQ_REMOVE(&victims, d_i, d_lru);
		assert(!kref_refcnt(&d_i->d_kref));
		__dentry_free(d_i);
	}
	/* It is possible at this point that there are new items on the LRU.  We
	 * could loop back until that list is empty, if we care about this. */
}

/* Inode Functions */

/* Creates and initializes a new inode.  Generic fields are filled in.
 * FS-specific fields are filled in by the callout.  Specific fields are filled
 * in in read_inode() based on what's on the disk for a given i_no, or when the
 * inode is created (for new objects).
 *
 * i_no is set by the caller.  Note that this means this inode can be for an
 * inode that is already on disk, or it can be used when creating. */
struct inode *get_inode(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	/* FS allocs and sets the following: i_op, i_fop, i_pm.pm_op, and any FS
	 * specific stuff. */
	struct inode *inode = sb->s_op->alloc_inode(sb);
	if (!inode) {
		set_errno(ENOMEM);
		return 0;
	}
	TAILQ_INSERT_HEAD(&sb->s_inodes, inode, i_sb_list);		/* weak inode ref */
	TAILQ_INIT(&inode->i_dentry);
	TAILQ_INSERT_TAIL(&inode->i_dentry, dentry, d_alias);	/* weak dentry ref*/
	/* one for the dentry->d_inode, one passed out */
	kref_init(&inode->i_kref, inode_release, 2);
	dentry->d_inode = inode;
	inode->i_ino = 0;					/* set by caller later */
	inode->i_blksize = sb->s_blocksize;
	spinlock_init(&inode->i_lock);
	kref_get(&sb->s_kref, 1);			/* could allow the dentry to pin it */
	inode->i_sb = sb;
	inode->i_rdev = 0;					/* this has no real meaning yet */
	inode->i_bdev = sb->s_bdev;			/* storing an uncounted ref */
	inode->i_state = 0;					/* need real states, like I_NEW */
	inode->dirtied_when = 0;
	inode->i_flags = 0;
	atomic_set(&inode->i_writecount, 0);
	/* Set up the page_map structures.  Default is to use the embedded one.
	 * Might push some of this back into specific FSs.  For now, the FS tells us
	 * what pm_op they want via i_pm.pm_op, which we set again in pm_init() */
	inode->i_mapping = &inode->i_pm;
	pm_init(inode->i_mapping, inode->i_pm.pm_op, inode);
	return inode;
}

/* Helper: loads/ reads in the inode numbered ino and attaches it to dentry */
void load_inode(struct dentry *dentry, unsigned long ino)
{
	struct inode *inode;

	/* look it up in the inode cache first */
	inode = icache_get(dentry->d_sb, ino);
	if (inode) {
		/* connect the dentry to its inode */
		TAILQ_INSERT_TAIL(&inode->i_dentry, dentry, d_alias);
		dentry->d_inode = inode;	/* storing the ref we got from icache_get */
		return;
	}
	/* otherwise, we need to do it manually */
	inode = get_inode(dentry);
	inode->i_ino = ino;
	dentry->d_sb->s_op->read_inode(inode);
	/* TODO: race here, two creators could miss in the cache, and then get here.
	 * need a way to sync across a blocking call.  needs to be either at this
	 * point in the code or per the ino (dentries could be different) */
	icache_put(dentry->d_sb, inode);
	kref_put(&inode->i_kref);
}

/* Helper op, used when creating regular files, directories, symlinks, etc.
 * Note we make a distinction between the mode and the file type (for now).
 * After calling this, call the FS specific version (create or mkdir), which
 * will set the i_ino, the filetype, and do any other FS-specific stuff.  Also
 * note that a lot of inode stuff was initialized in get_inode/alloc_inode.  The
 * stuff here is pertinent to the specific creator (user), mode, and time.  Also
 * note we don't pass this an nd, like Linux does... */
static struct inode *create_inode(struct dentry *dentry, int mode)
{
	uint64_t now = epoch_seconds();
	/* note it is the i_ino that uniquely identifies a file in the specific
	 * filesystem.  there's a diff between creating an inode (even for an in-use
	 * ino) and then filling it in, and vs creating a brand new one.
	 * get_inode() sets it to 0, and it should be filled in later in an
	 * FS-specific manner. */
	struct inode *inode = get_inode(dentry);
	if (!inode)
		return 0;
	inode->i_mode = mode & S_PMASK;	/* note that after this, we have no type */
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_atime.tv_sec = now;
	inode->i_ctime.tv_sec = now;
	inode->i_mtime.tv_sec = now;
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_bdev = inode->i_sb->s_bdev;
	/* when we have notions of users, do something here: */
	inode->i_uid = 0;
	inode->i_gid = 0;
	return inode;
}

/* Create a new disk inode in dir associated with dentry, with the given mode.
 * called when creating a regular file.  dir is the directory/parent.  dentry is
 * the dentry of the inode we are creating.  Note the lack of the nd... */
int create_file(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *new_file = create_inode(dentry, mode);
	if (!new_file)
		return -1;
	dir->i_op->create(dir, dentry, mode, 0);
	icache_put(new_file->i_sb, new_file);
	kref_put(&new_file->i_kref);
	return 0;
}

/* Creates a new inode for a directory associated with dentry in dir with the
 * given mode. */
int create_dir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *new_dir = create_inode(dentry, mode);
	if (!new_dir)
		return -1;
	dir->i_op->mkdir(dir, dentry, mode);
	dir->i_nlink++;		/* Directories get a hardlink for every child dir */
	/* Make sure my parent tracks me.  This is okay, since no directory (dir)
	 * can have more than one dentry */
	struct dentry *parent = TAILQ_FIRST(&dir->i_dentry);
	assert(parent && parent == TAILQ_LAST(&dir->i_dentry, dentry_tailq));
	/* parent dentry tracks dentry as a subdir, weak reference */
	TAILQ_INSERT_TAIL(&parent->d_subdirs, dentry, d_subdirs_link);
	icache_put(new_dir->i_sb, new_dir);
	kref_put(&new_dir->i_kref);
	return 0;
}

/* Creates a new inode for a symlink associated with dentry in dir, containing
 * the symlink symname */
int create_symlink(struct inode *dir, struct dentry *dentry,
                   const char *symname, int mode)
{
	struct inode *new_sym = create_inode(dentry, mode);
	if (!new_sym)
		return -1;
	dir->i_op->symlink(dir, dentry, symname);
	icache_put(new_sym->i_sb, new_sym);
	kref_put(&new_sym->i_kref);
	return 0;
}

/* Returns 0 if the given mode is acceptable for the inode, and an appropriate
 * error code if not.  Needs to be writen, based on some sensible rules, and
 * will also probably use 'current' */
int check_perms(struct inode *inode, int access_mode)
{
	return 0;	/* anything goes! */
}

/* Called after all external refs are gone to clean up the inode.  Once this is
 * called, all dentries pointing here are already done (one of them triggered
 * this via kref_put(). */
void inode_release(struct kref *kref)
{
	struct inode *inode = container_of(kref, struct inode, i_kref);
	TAILQ_REMOVE(&inode->i_sb->s_inodes, inode, i_sb_list);
	icache_remove(inode->i_sb, inode->i_ino);
	/* Might need to write back or delete the file/inode */
	if (inode->i_nlink) {
		if (inode->i_state & I_STATE_DIRTY)
			inode->i_sb->s_op->write_inode(inode, TRUE);
	} else {
		inode->i_sb->s_op->delete_inode(inode);
	}
	if (S_ISFIFO(inode->i_mode)) {
		page_decref(kva2page(inode->i_pipe->p_buf));
		kfree(inode->i_pipe);
	}
	/* TODO: (BDEV) */
	// kref_put(inode->i_bdev->kref); /* assuming it's a bdev, could be a pipe*/
	/* Either way, we dealloc the in-memory version */
	inode->i_sb->s_op->dealloc_inode(inode);	/* FS-specific clean-up */
	kref_put(&inode->i_sb->s_kref);
	/* TODO: clean this up */
	assert(inode->i_mapping == &inode->i_pm);
	kmem_cache_free(inode_kcache, inode);
}

/* Fills in kstat with the stat information for the inode */
void stat_inode(struct inode *inode, struct kstat *kstat)
{
	kstat->st_dev = inode->i_sb->s_dev;
	kstat->st_ino = inode->i_ino;
	kstat->st_mode = inode->i_mode;
	kstat->st_nlink = inode->i_nlink;
	kstat->st_uid = inode->i_uid;
	kstat->st_gid = inode->i_gid;
	kstat->st_rdev = inode->i_rdev;
	kstat->st_size = inode->i_size;
	kstat->st_blksize = inode->i_blksize;
	kstat->st_blocks = inode->i_blocks;
	kstat->st_atime = inode->i_atime;
	kstat->st_mtime = inode->i_mtime;
	kstat->st_ctime = inode->i_ctime;
}

void print_kstat(struct kstat *kstat)
{
	printk("kstat info for %p:\n", kstat);
	printk("\tst_dev    : %p\n", kstat->st_dev);
	printk("\tst_ino    : %p\n", kstat->st_ino);
	printk("\tst_mode   : %p\n", kstat->st_mode);
	printk("\tst_nlink  : %p\n", kstat->st_nlink);
	printk("\tst_uid    : %p\n", kstat->st_uid);
	printk("\tst_gid    : %p\n", kstat->st_gid);
	printk("\tst_rdev   : %p\n", kstat->st_rdev);
	printk("\tst_size   : %p\n", kstat->st_size);
	printk("\tst_blksize: %p\n", kstat->st_blksize);
	printk("\tst_blocks : %p\n", kstat->st_blocks);
	printk("\tst_atime  : %p\n", kstat->st_atime);
	printk("\tst_mtime  : %p\n", kstat->st_mtime);
	printk("\tst_ctime  : %p\n", kstat->st_ctime);
}

/* Inode Cache management.  In general, search on the ino, get a refcnt'd value
 * back.  Remove does not give you a reference back - it should only be called
 * in inode_release(). */
struct inode *icache_get(struct super_block *sb, unsigned long ino)
{
	/* This is the same style as in pid2proc, it's the "safely create a strong
	 * reference from a weak one, so long as other strong ones exist" pattern */
	spin_lock(&sb->s_icache_lock);
	struct inode *inode = hashtable_search(sb->s_icache, (void*)ino);
	if (inode)
		if (!kref_get_not_zero(&inode->i_kref, 1))
			inode = 0;
	spin_unlock(&sb->s_icache_lock);
	return inode;
}

void icache_put(struct super_block *sb, struct inode *inode)
{
	spin_lock(&sb->s_icache_lock);
	/* there's a race in load_ino() that could trigger this */
	assert(!hashtable_search(sb->s_icache, (void*)inode->i_ino));
	hashtable_insert(sb->s_icache, (void*)inode->i_ino, inode);
	spin_unlock(&sb->s_icache_lock);
}

struct inode *icache_remove(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	/* Presumably these hashtable removals could be easier since callers
	 * actually know who they are (same with the pid2proc hash) */
	spin_lock(&sb->s_icache_lock);
	inode = hashtable_remove(sb->s_icache, (void*)ino);
	spin_unlock(&sb->s_icache_lock);
	assert(inode && !kref_refcnt(&inode->i_kref));
	return inode;
}

/* File functions */

/* Read count bytes from the file into buf, starting at *offset, which is
 * increased accordingly, returning the number of bytes transfered.  Most
 * filesystems will use this function for their f_op->read.
 * Note, this uses the page cache. */
ssize_t generic_file_read(struct file *file, char *buf, size_t count,
                          off64_t *offset)
{
	struct page *page;
	int error;
	off64_t page_off;
	unsigned long first_idx, last_idx;
	size_t copy_amt;
	char *buf_end;
	/* read in offset, in case of a concurrent reader/writer, so we don't screw
	 * up our math for count, the idxs, etc. */
	off64_t orig_off = ACCESS_ONCE(*offset);

	/* Consider pushing some error checking higher in the VFS */
	if (!count)
		return 0;
	if (orig_off >= file->f_dentry->d_inode->i_size)
		return 0; /* EOF */
	/* Make sure we don't go past the end of the file */
	if (orig_off + count > file->f_dentry->d_inode->i_size) {
		count = file->f_dentry->d_inode->i_size - orig_off;
	}
	assert((long)count > 0);
	page_off = orig_off & (PGSIZE - 1);
	first_idx = orig_off >> PGSHIFT;
	last_idx = (orig_off + count) >> PGSHIFT;
	buf_end = buf + count;
	/* For each file page, make sure it's in the page cache, then copy it out.
	 * TODO: will probably need to consider concurrently truncated files here.*/
	for (int i = first_idx; i <= last_idx; i++) {
		error = pm_load_page(file->f_mapping, i, &page);
		assert(!error);	/* TODO: handle ENOMEM and friends */
		copy_amt = MIN(PGSIZE - page_off, buf_end - buf);
		/* TODO: (UMEM) think about this.  if it's a user buffer, we're relying
		 * on current to detect whose it is (which should work for async calls).
		 * Also, need to propagate errors properly...  Probably should do a
		 * user_mem_check, then free, and also to make a distinction between
		 * when the kernel wants a read/write (TODO: KFOP) */
		if (current) {
			memcpy_to_user(current, buf, page2kva(page) + page_off, copy_amt);
		} else {
			memcpy(buf, page2kva(page) + page_off, copy_amt);
		}
		buf += copy_amt;
		page_off = 0;
		pm_put_page(page);	/* it's still in the cache, we just don't need it */
	}
	assert(buf == buf_end);
	/* could have concurrent file ops that screw with offset, so userspace isn't
	 * safe.  but at least it'll be a value that one of the concurrent ops could
	 * have produced (compared to *offset_changed_concurrently += count. */
	*offset = orig_off + count;
	return count;
}

/* Write count bytes from buf to the file, starting at *offset, which is
 * increased accordingly, returning the number of bytes transfered.  Most
 * filesystems will use this function for their f_op->write.  Note, this uses
 * the page cache.
 *
 * Changes don't get flushed to disc til there is an fsync, page cache eviction,
 * or other means of trying to writeback the pages. */
ssize_t generic_file_write(struct file *file, const char *buf, size_t count,
                           off64_t *offset)
{
	struct page *page;
	int error;
	off64_t page_off;
	unsigned long first_idx, last_idx;
	size_t copy_amt;
	const char *buf_end;
	off64_t orig_off = ACCESS_ONCE(*offset);

	/* Consider pushing some error checking higher in the VFS */
	if (!count)
		return 0;
	if (file->f_flags & O_APPEND) {
		spin_lock(&file->f_dentry->d_inode->i_lock);
		orig_off = file->f_dentry->d_inode->i_size;
		/* setting the filesize here, instead of during the extend-check, since
		 * we need to atomically reserve space and set our write position. */
		file->f_dentry->d_inode->i_size += count;
		spin_unlock(&file->f_dentry->d_inode->i_lock);
	} else {
		if (orig_off + count > file->f_dentry->d_inode->i_size) {
			/* lock for writes to i_size.  we allow lockless reads.  recheck
			 * i_size in case of concurrent writers since our orig check.  */
			spin_lock(&file->f_dentry->d_inode->i_lock);
			if (orig_off + count > file->f_dentry->d_inode->i_size)
				file->f_dentry->d_inode->i_size = orig_off + count;
			spin_unlock(&file->f_dentry->d_inode->i_lock);
		}
	}
	page_off = orig_off & (PGSIZE - 1);
	first_idx = orig_off >> PGSHIFT;
	last_idx = (orig_off + count) >> PGSHIFT;
	buf_end = buf + count;
	/* For each file page, make sure it's in the page cache, then write it.*/
	for (int i = first_idx; i <= last_idx; i++) {
		error = pm_load_page(file->f_mapping, i, &page);
		assert(!error);	/* TODO: handle ENOMEM and friends */
		copy_amt = MIN(PGSIZE - page_off, buf_end - buf);
		/* TODO: (UMEM) (KFOP) think about this.  if it's a user buffer, we're
		 * relying on current to detect whose it is (which should work for async
		 * calls). */
		if (current) {
			memcpy_from_user(current, page2kva(page) + page_off, buf, copy_amt);
		} else {
			memcpy(page2kva(page) + page_off, buf, copy_amt);
		}
		buf += copy_amt;
		page_off = 0;
		atomic_or(&page->pg_flags, PG_DIRTY);
		pm_put_page(page);	/* it's still in the cache, we just don't need it */
	}
	assert(buf == buf_end);
	*offset = orig_off + count;
	return count;
}

/* Directories usually use this for their read method, which is the way glibc
 * currently expects us to do a readdir (short of doing linux's getdents).  Will
 * probably need work, based on whatever real programs want. */
ssize_t generic_dir_read(struct file *file, char *u_buf, size_t count,
                         off64_t *offset)
{
	struct kdirent dir_r = {0}, *dirent = &dir_r;
	int retval = 1;
	size_t amt_copied = 0;
	char *buf_end = u_buf + count;

	if (!S_ISDIR(file->f_dentry->d_inode->i_mode)) {
		set_errno(ENOTDIR);
		return -1;
	}
	if (!count)
		return 0;
	/* start readdir from where it left off: */
	dirent->d_off = *offset;
	for (   ;
	        u_buf + sizeof(struct kdirent) <= buf_end;
	        u_buf += sizeof(struct kdirent)) {
		/* TODO: UMEM/KFOP (pin the u_buf in the syscall, ditch the local copy,
		 * get rid of this memcpy and reliance on current, etc).  Might be
		 * tricky with the dirent->d_off and trust issues */
		retval = file->f_op->readdir(file, dirent);
		if (retval < 0) {
			set_errno(-retval);
			break;
		}
		/* Slight info exposure: could be extra crap after the name in the
		 * dirent (like the name of a deleted file) */
		if (current) {
			memcpy_to_user(current, u_buf, dirent, sizeof(struct dirent));
		} else {
			memcpy(u_buf, dirent, sizeof(struct dirent));
		}
		amt_copied += sizeof(struct dirent);
		/* 0 signals end of directory */
		if (retval == 0)
			break;
	}
	/* Next time read is called, we pick up where we left off */
	*offset = dirent->d_off;	/* UMEM */
	/* important to tell them how much they got.  they often keep going til they
	 * get 0 back (in the case of ls).  it's also how much has been read, but it
	 * isn't how much the f_pos has moved (which is opaque to the VFS). */
	return amt_copied;
}

/* Opens the file, using permissions from current for lack of a better option.
 * It will attempt to create the file if it does not exist and O_CREAT is
 * specified.  This will return 0 on failure, and set errno.  TODO: There's some
 * stuff that we don't do, esp related file truncating/creation.  flags are for
 * opening, the mode is for creating.  The flags related to how to create
 * (O_CREAT_FLAGS) are handled in this function, not in create_file().
 *
 * It's tempting to split this into a do_file_create and a do_file_open, based
 * on the O_CREAT flag, but the O_CREAT flag can be ignored if the file exists
 * already and O_EXCL isn't specified.  We could have open call create if it
 * fails, but for now we'll keep it as is. */
struct file *do_file_open(char *path, int flags, int mode)
{
	struct file *file = 0;
	struct dentry *file_d;
	struct inode *parent_i;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	unsigned long nr_pages;

	/* The file might exist, lets try to just open it right away */
	nd->intent = LOOKUP_OPEN;
	error = path_lookup(path, LOOKUP_FOLLOW, nd);
	if (!error) {
		/* If this is a directory, make sure we are opening with O_RDONLY.
		 * Unfortunately we can't just check for O_RDONLY directly because its
		 * value is 0x0.  We instead have to make sure it's not O_WRONLY and
		 * not O_RDWR explicitly. */
		if (S_ISDIR(nd->dentry->d_inode->i_mode) &&
		    ((flags & O_WRONLY) || (flags & O_RDWR))) {
			set_errno(EISDIR);
			goto out_path_only;
		}
		/* Also need to make sure we didn't want to O_EXCL create */
		if ((flags & O_CREAT) && (flags & O_EXCL)) {
			set_errno(EEXIST);
			goto out_path_only;
		}
		file_d = nd->dentry;
		kref_get(&file_d->d_kref, 1);
		goto open_the_file;
	}
	if (!(flags & O_CREAT)) {
		set_errno(-error);
		goto out_path_only;
	}
	/* So it didn't already exist, release the path from the previous lookup,
	 * and then we try to create it. */
	path_release(nd);	
	/* get the parent, following links.  this means you get the parent of the
	 * final link (which may not be in 'path' in the first place. */
	nd->intent = LOOKUP_CREATE;
	error = path_lookup(path, LOOKUP_PARENT | LOOKUP_FOLLOW, nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	/* see if the target is there (shouldn't be), and handle accordingly */
	file_d = do_lookup(nd->dentry, nd->last.name); 
	if (!file_d) {
		if (!(flags & O_CREAT)) {
			warn("Extremely unlikely race, probably a bug");
			set_errno(ENOENT);
			goto out_path_only;
		}
		/* Create the inode/file.  get a fresh dentry too: */
		file_d = get_dentry(nd->dentry->d_sb, nd->dentry, nd->last.name);
		if (!file_d)
			goto out_path_only;
		parent_i = nd->dentry->d_inode;
		/* Note that the mode technically should only apply to future opens,
		 * but we apply it immediately. */
		if (create_file(parent_i, file_d, mode))	/* sets errno */
			goto out_file_d;
		dcache_put(file_d->d_sb, file_d);
	} else {	/* something already exists */
		/* this can happen due to concurrent access, but needs to be thought
		 * through */
		panic("File shouldn't be here!");
		if ((flags & O_CREAT) && (flags & O_EXCL)) {
			/* wanted to create, not open, bail out */
			set_errno(EEXIST);
			goto out_file_d;
		}
	}
open_the_file:
	/* now open the file (freshly created or if it already existed).  At this
	 * point, file_d is a refcnt'd dentry, regardless of which branch we took.*/
	if (flags & O_TRUNC) {
		spin_lock(&file_d->d_inode->i_lock);
		nr_pages = ROUNDUP(file_d->d_inode->i_size, PGSIZE) >> PGSHIFT;
		file_d->d_inode->i_size = 0;
		spin_unlock(&file_d->d_inode->i_lock);
		pm_remove_contig(file_d->d_inode->i_mapping, 0, nr_pages);
	}
	file = dentry_open(file_d, flags);				/* sets errno */
	/* Note the fall through to the exit paths.  File is 0 by default and if
	 * dentry_open fails. */
out_file_d:
	kref_put(&file_d->d_kref);
out_path_only:
	path_release(nd);
	return file;
}

/* Path is the location of the symlink, sometimes called the "new path", and
 * symname is who we link to, sometimes called the "old path". */
int do_symlink(char *path, const char *symname, int mode)
{
	struct dentry *sym_d;
	struct inode *parent_i;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	int retval = -1;

	nd->intent = LOOKUP_CREATE;
	/* get the parent, but don't follow links */
	error = path_lookup(path, LOOKUP_PARENT, nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	/* see if the target is already there, handle accordingly */
	sym_d = do_lookup(nd->dentry, nd->last.name); 
	if (sym_d) {
		set_errno(EEXIST);
		goto out_sym_d;
	}
	/* Doesn't already exist, let's try to make it: */
	sym_d = get_dentry(nd->dentry->d_sb, nd->dentry, nd->last.name);
	if (!sym_d)
		goto out_path_only;
	parent_i = nd->dentry->d_inode;
	if (create_symlink(parent_i, sym_d, symname, mode))
		goto out_sym_d;
	dcache_put(sym_d->d_sb, sym_d);
	retval = 0;				/* Note the fall through to the exit paths */
out_sym_d:
	kref_put(&sym_d->d_kref);
out_path_only:
	path_release(nd);
	return retval;
}

/* Makes a hard link for the file behind old_path to new_path */
int do_link(char *old_path, char *new_path)
{
	struct dentry *link_d, *old_d;
	struct inode *inode, *parent_dir;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	int retval = -1;

	nd->intent = LOOKUP_CREATE;
	/* get the absolute parent of the new_path */
	error = path_lookup(new_path, LOOKUP_PARENT | LOOKUP_FOLLOW, nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	parent_dir = nd->dentry->d_inode;
	/* see if the new target is already there, handle accordingly */
	link_d = do_lookup(nd->dentry, nd->last.name); 
	if (link_d) {
		set_errno(EEXIST);
		goto out_link_d;
	}
	/* Doesn't already exist, let's try to make it.  Still need to stitch it to
	 * an inode and set its FS-specific stuff after this.*/
	link_d = get_dentry(nd->dentry->d_sb, nd->dentry, nd->last.name);
	if (!link_d)
		goto out_path_only;
	/* Now let's get the old_path target */
	old_d = lookup_dentry(old_path, LOOKUP_FOLLOW);
	if (!old_d)					/* errno set by lookup_dentry */
		goto out_link_d;
	/* For now, can only link to files */
	if (!S_ISREG(old_d->d_inode->i_mode)) {
		set_errno(EPERM);
		goto out_both_ds;
	}
	/* Must be on the same FS */
	if (old_d->d_sb != link_d->d_sb) {
		set_errno(EXDEV);
		goto out_both_ds;
	}
	/* Do whatever FS specific stuff there is first (which is also a chance to
	 * bail out). */
	error = parent_dir->i_op->link(old_d, parent_dir, link_d);
	if (error) {
		set_errno(-error);
		goto out_both_ds;
	}
	/* Finally stitch it up */
	inode = old_d->d_inode;
	kref_get(&inode->i_kref, 1);
	link_d->d_inode = inode;
	inode->i_nlink++;
	TAILQ_INSERT_TAIL(&inode->i_dentry, link_d, d_alias);	/* weak ref */
	dcache_put(link_d->d_sb, link_d);
	retval = 0;				/* Note the fall through to the exit paths */
out_both_ds:
	kref_put(&old_d->d_kref);
out_link_d:
	kref_put(&link_d->d_kref);
out_path_only:
	path_release(nd);
	return retval;
}

/* Unlinks path from the directory tree.  Read the Documentation for more info.
 */
int do_unlink(char *path)
{
	struct dentry *dentry;
	struct inode *parent_dir;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	int retval = -1;

	/* get the parent of the target, and don't follow a final link */
	error = path_lookup(path, LOOKUP_PARENT, nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	parent_dir = nd->dentry->d_inode;
	/* make sure the target is there */
	dentry = do_lookup(nd->dentry, nd->last.name); 
	if (!dentry) {
		set_errno(ENOENT);
		goto out_path_only;
	}
	/* Make sure the target is not a directory */
	if (S_ISDIR(dentry->d_inode->i_mode)) {
		set_errno(EISDIR);
		goto out_dentry;
	}
	/* Remove the dentry from its parent */
	error = parent_dir->i_op->unlink(parent_dir, dentry);
	if (error) {
		set_errno(-error);
		goto out_dentry;
	}
	/* Now that our parent doesn't track us, we need to make sure we aren't
	 * findable via the dentry cache.  DYING, so we will be freed in
	 * dentry_release() */
	dentry->d_flags |= DENTRY_DYING;
	dcache_remove(dentry->d_sb, dentry);
	dentry->d_inode->i_nlink--;	/* TODO: race here, esp with a decref */
	/* At this point, the dentry is unlinked from the FS, and the inode has one
	 * less link.  When the in-memory objects (dentry, inode) are going to be
	 * released (after all open files are closed, and maybe after entries are
	 * evicted from the cache), then nlinks will get checked and the FS-file
	 * will get removed from the disk */
	retval = 0;				/* Note the fall through to the exit paths */
out_dentry:
	kref_put(&dentry->d_kref);
out_path_only:
	path_release(nd);
	return retval;
}

/* Checks to see if path can be accessed via mode.  Need to actually send the
 * mode along somehow, so this doesn't do much now.  This is an example of
 * decent error propagation from the lower levels via int retvals. */
int do_access(char *path, int mode)
{
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int retval = 0;
	nd->intent = LOOKUP_ACCESS;
	retval = path_lookup(path, 0, nd);
	path_release(nd);	
	return retval;
}

int do_file_chmod(struct file *file, int mode)
{
	int old_mode_ftype = file->f_dentry->d_inode->i_mode & __S_IFMT;
	#if 0
	/* TODO: when we have notions of uid, check for the proc's uid */
	if (file->f_dentry->d_inode->i_uid != UID_OF_ME)
		retval = -EPERM;
	else
	#endif
		file->f_dentry->d_inode->i_mode = (mode & S_PMASK) | old_mode_ftype;
	return 0;
}

/* Make a directory at path with mode.  Returns -1 and sets errno on errors */
int do_mkdir(char *path, int mode)
{
	struct dentry *dentry;
	struct inode *parent_i;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	int retval = -1;

	nd->intent = LOOKUP_CREATE;
	/* get the parent, but don't follow links */
	error = path_lookup(path, LOOKUP_PARENT, nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	/* see if the target is already there, handle accordingly */
	dentry = do_lookup(nd->dentry, nd->last.name); 
	if (dentry) {
		set_errno(EEXIST);
		goto out_dentry;
	}
	/* Doesn't already exist, let's try to make it: */
	dentry = get_dentry(nd->dentry->d_sb, nd->dentry, nd->last.name);
	if (!dentry)
		goto out_path_only;
	parent_i = nd->dentry->d_inode;
	if (create_dir(parent_i, dentry, mode))
		goto out_dentry;
	dcache_put(dentry->d_sb, dentry);
	retval = 0;				/* Note the fall through to the exit paths */
out_dentry:
	kref_put(&dentry->d_kref);
out_path_only:
	path_release(nd);
	return retval;
}

int do_rmdir(char *path)
{
	struct dentry *dentry;
	struct inode *parent_i;
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	int retval = -1;

	/* get the parent, following links (probably want this), and we must get a
	 * directory.  Note, current versions of path_lookup can't handle both
	 * PARENT and DIRECTORY, at least, it doesn't check that *path is a
	 * directory. */
	error = path_lookup(path, LOOKUP_PARENT | LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
	                    nd);
	if (error) {
		set_errno(-error);
		goto out_path_only;
	}
	/* make sure the target is already there, handle accordingly */
	dentry = do_lookup(nd->dentry, nd->last.name); 
	if (!dentry) {
		set_errno(ENOENT);
		goto out_path_only;
	}
	if (!S_ISDIR(dentry->d_inode->i_mode)) {
		set_errno(ENOTDIR);
		goto out_dentry;
	}
	if (dentry->d_mount_point) {
		set_errno(EBUSY);
		goto out_dentry;
	}
	/* TODO: make sure we aren't a mount or processes root (EBUSY) */
	/* Now for the removal.  the FSs will check if they are empty */
	parent_i = nd->dentry->d_inode;
	error = parent_i->i_op->rmdir(parent_i, dentry);
	if (error < 0) {
		set_errno(-error);
		goto out_dentry;
	}
	/* Now that our parent doesn't track us, we need to make sure we aren't
	 * findable via the dentry cache.  DYING, so we will be freed in
	 * dentry_release() */
	dentry->d_flags |= DENTRY_DYING;
	dcache_remove(dentry->d_sb, dentry);
	/* Decref ourselves, so inode_release() knows we are done */
	dentry->d_inode->i_nlink--;
	TAILQ_REMOVE(&nd->dentry->d_subdirs, dentry, d_subdirs_link);
	parent_i->i_nlink--;		/* TODO: race on this, esp since its a decref */
	/* we still have d_parent and a kref on our parent, which will go away when
	 * the in-memory dentry object goes away. */
	retval = 0;				/* Note the fall through to the exit paths */
out_dentry:
	kref_put(&dentry->d_kref);
out_path_only:
	path_release(nd);
	return retval;
}

/* Pipes: Doing a simple buffer with reader and writer offsets.  Size is power
 * of two, so we can easily compute its status and whatnot. */

#define PIPE_SZ					(1 << PGSHIFT)

static size_t pipe_get_rd_idx(struct pipe_inode_info *pii)
{
	return pii->p_rd_off & (PIPE_SZ - 1);
}

static size_t pipe_get_wr_idx(struct pipe_inode_info *pii)
{

	return pii->p_wr_off & (PIPE_SZ - 1);
}

static bool pipe_is_empty(struct pipe_inode_info *pii)
{
	return __ring_empty(pii->p_wr_off, pii->p_rd_off);
}

static bool pipe_is_full(struct pipe_inode_info *pii)
{
	return __ring_full(PIPE_SZ, pii->p_wr_off, pii->p_rd_off);
}

static size_t pipe_nr_full(struct pipe_inode_info *pii)
{
	return __ring_nr_full(pii->p_wr_off, pii->p_rd_off);
}

static size_t pipe_nr_empty(struct pipe_inode_info *pii)
{
	return __ring_nr_empty(PIPE_SZ, pii->p_wr_off, pii->p_rd_off);
}

ssize_t pipe_file_read(struct file *file, char *buf, size_t count,
                       off64_t *offset)
{
	struct pipe_inode_info *pii = file->f_dentry->d_inode->i_pipe;
	size_t copy_amt, amt_copied = 0;

	cv_lock(&pii->p_cv);
	while (pipe_is_empty(pii)) {
		/* We wait til the pipe is drained before sending EOF if there are no
		 * writers (instead of aborting immediately) */
		if (!pii->p_nr_writers) {
			cv_unlock(&pii->p_cv);
			return 0;
		}
		if (file->f_flags & O_NONBLOCK) {
			cv_unlock(&pii->p_cv);
			set_errno(EAGAIN);
			return -1;
		}
		cv_wait(&pii->p_cv);
		cpu_relax();
	}
	/* We might need to wrap-around with our copy, so we'll do the copy in two
	 * passes.  This will copy up to the end of the buffer, then on the next
	 * pass will copy the rest to the beginning of the buffer (if necessary) */
	for (int i = 0; i < 2; i++) {
		copy_amt = MIN(PIPE_SZ - pipe_get_rd_idx(pii),
		               MIN(pipe_nr_full(pii), count));
		assert(current);	/* shouldn't pipe from the kernel */
		memcpy_to_user(current, buf, pii->p_buf + pipe_get_rd_idx(pii),
		               copy_amt);
		buf += copy_amt;
		count -= copy_amt;
		pii->p_rd_off += copy_amt;
		amt_copied += copy_amt;
	}
	/* Just using one CV for both readers and writers.  We should rarely have
	 * multiple readers or writers. */
	if (amt_copied)
		__cv_broadcast(&pii->p_cv);
	cv_unlock(&pii->p_cv);
	return amt_copied;
}

/* Note: we're not dealing with PIPE_BUF and minimum atomic chunks, unless I
 * have to later. */
ssize_t pipe_file_write(struct file *file, const char *buf, size_t count,
                        off64_t *offset)
{
	struct pipe_inode_info *pii = file->f_dentry->d_inode->i_pipe;
	size_t copy_amt, amt_copied = 0;

	cv_lock(&pii->p_cv);
	/* Write aborts right away if there are no readers, regardless of pipe
	 * status. */
	if (!pii->p_nr_readers) {
		cv_unlock(&pii->p_cv);
		set_errno(EPIPE);
		return -1;
	}
	while (pipe_is_full(pii)) {
		if (file->f_flags & O_NONBLOCK) {
			cv_unlock(&pii->p_cv);
			set_errno(EAGAIN);
			return -1;
		}
		cv_wait(&pii->p_cv);
		cpu_relax();
		/* Still need to check in the loop, in case the last reader left while
		 * we slept. */
		if (!pii->p_nr_readers) {
			cv_unlock(&pii->p_cv);
			set_errno(EPIPE);
			return -1;
		}
	}
	/* We might need to wrap-around with our copy, so we'll do the copy in two
	 * passes.  This will copy up to the end of the buffer, then on the next
	 * pass will copy the rest to the beginning of the buffer (if necessary) */
	for (int i = 0; i < 2; i++) {
		copy_amt = MIN(PIPE_SZ - pipe_get_wr_idx(pii),
		               MIN(pipe_nr_empty(pii), count));
		assert(current);	/* shouldn't pipe from the kernel */
		memcpy_from_user(current, pii->p_buf + pipe_get_wr_idx(pii), buf,
		                 copy_amt);
		buf += copy_amt;
		count -= copy_amt;
		pii->p_wr_off += copy_amt;
		amt_copied += copy_amt;
	}
	/* Just using one CV for both readers and writers.  We should rarely have
	 * multiple readers or writers. */
	if (amt_copied)
		__cv_broadcast(&pii->p_cv);
	cv_unlock(&pii->p_cv);
	return amt_copied;
}

/* In open and release, we need to track the number of readers and writers,
 * which we can differentiate by the file flags. */
int pipe_open(struct inode *inode, struct file *file)
{
	struct pipe_inode_info *pii = inode->i_pipe;
	cv_lock(&pii->p_cv);
	/* Ugliness due to not using flags for O_RDONLY and friends... */
	if (file->f_mode == S_IRUSR) {
		pii->p_nr_readers++;
	} else if (file->f_mode == S_IWUSR) {
		pii->p_nr_writers++;
	} else {
		warn("Bad pipe file flags 0x%x\n", file->f_flags);
	}
	cv_unlock(&pii->p_cv);
	return 0;
}

int pipe_release(struct inode *inode, struct file *file)
{
	struct pipe_inode_info *pii = inode->i_pipe;
	cv_lock(&pii->p_cv);
	/* Ugliness due to not using flags for O_RDONLY and friends... */
	if (file->f_mode == S_IRUSR) {
		pii->p_nr_readers--;
	} else if (file->f_mode == S_IWUSR) {
		pii->p_nr_writers--;
	} else {
		warn("Bad pipe file flags 0x%x\n", file->f_flags);
	}
	/* need to wake up any sleeping readers/writers, since we might be done */
	__cv_broadcast(&pii->p_cv);
	cv_unlock(&pii->p_cv);
	return 0;
}

struct file_operations pipe_f_op = {
	.read = pipe_file_read,
	.write = pipe_file_write,
	.open = pipe_open,
	.release = pipe_release,
	0
};

void pipe_debug(struct file *f)
{
	struct pipe_inode_info *pii = f->f_dentry->d_inode->i_pipe;
	assert(pii);
	printk("PIPE %p\n", pii);
	printk("\trdoff %p\n", pii->p_rd_off);
	printk("\twroff %p\n", pii->p_wr_off);
	printk("\tnr_rds %d\n", pii->p_nr_readers);
	printk("\tnr_wrs %d\n", pii->p_nr_writers);
	printk("\tcv waiters %d\n", pii->p_cv.nr_waiters);

}

/* General plan: get a dentry/inode to represent the pipe.  We'll alloc it from
 * the default_ns SB, but won't actually link it anywhere.  It'll only be held
 * alive by the krefs, til all the FDs are closed. */
int do_pipe(struct file **pipe_files, int flags)
{
	struct dentry *pipe_d;
	struct inode *pipe_i;
	struct file *pipe_f_read, *pipe_f_write;
	struct super_block *def_sb = default_ns.root->mnt_sb;
	struct pipe_inode_info *pii;

	pipe_d = get_dentry(def_sb, 0, "pipe");
	if (!pipe_d)
		return -1;
	pipe_d->d_op = &dummy_d_op;
	pipe_i = get_inode(pipe_d);
	if (!pipe_i)
		goto error_post_dentry;
	/* preemptively mark the dentry for deletion.  we have an unlinked dentry
	 * right off the bat, held in only by the kref chain (pipe_d is the ref). */
	pipe_d->d_flags |= DENTRY_DYING;
	/* pipe_d->d_inode still has one ref to pipe_i, keeping the inode alive */
	kref_put(&pipe_i->i_kref);
	/* init inode fields.  note we're using the dummy ops for i_op and d_op */
	pipe_i->i_mode = S_IRWXU | S_IRWXG | S_IRWXO;
	SET_FTYPE(pipe_i->i_mode, __S_IFIFO);	/* using type == FIFO */
	pipe_i->i_nlink = 1;			/* one for the dentry */
	pipe_i->i_uid = 0;
	pipe_i->i_gid = 0;
	pipe_i->i_size = PGSIZE;
	pipe_i->i_blocks = 0;
	pipe_i->i_atime.tv_sec = 0;
	pipe_i->i_atime.tv_nsec = 0;
	pipe_i->i_mtime.tv_sec = 0;
	pipe_i->i_mtime.tv_nsec = 0;
	pipe_i->i_ctime.tv_sec = 0;
	pipe_i->i_ctime.tv_nsec = 0;
	pipe_i->i_fs_info = 0;
	pipe_i->i_op = &dummy_i_op;
	pipe_i->i_fop = &pipe_f_op;
	pipe_i->i_socket = FALSE;
	/* Actually build the pipe.  We're using one page, hanging off the
	 * pipe_inode_info struct.  When we release the inode, we free the pipe
	 * memory too */
	pipe_i->i_pipe = kmalloc(sizeof(struct pipe_inode_info), KMALLOC_WAIT);
	pii = pipe_i->i_pipe;
	if (!pii) {
		set_errno(ENOMEM);
		goto error_kmalloc;
	}
	pii->p_buf = kpage_zalloc_addr();
	if (!pii->p_buf) {
		set_errno(ENOMEM);
		goto error_kpage;
	}
	pii->p_rd_off = 0;
	pii->p_wr_off = 0;
	pii->p_nr_readers = 0;
	pii->p_nr_writers = 0;
	cv_init(&pii->p_cv);	/* must do this before dentry_open / pipe_open */
	/* Now we have an inode for the pipe.  We need two files for the read and
	 * write ends of the pipe. */
	flags &= ~(O_ACCMODE);	/* avoid user bugs */
	pipe_f_read = dentry_open(pipe_d, flags | O_RDONLY);
	if (!pipe_f_read)
		goto error_f_read;
	pipe_f_write = dentry_open(pipe_d, flags | O_WRONLY);
	if (!pipe_f_write)
		goto error_f_write;
	pipe_files[0] = pipe_f_read;
	pipe_files[1] = pipe_f_write;
	return 0;

error_f_write:
	kref_put(&pipe_f_read->f_kref);
error_f_read:
	page_decref(kva2page(pii->p_buf));
error_kpage:
	kfree(pipe_i->i_pipe);
error_kmalloc:
	/* We don't need to free the pipe_i; putting the dentry will free it */
error_post_dentry:
	/* Note we only free the dentry on failure. */
	kref_put(&pipe_d->d_kref);
	return -1;
}

int do_rename(char *old_path, char *new_path)
{
	struct nameidata nd_old = {0}, *nd_o = &nd_old;
	struct nameidata nd_new = {0}, *nd_n = &nd_new;
	struct dentry *old_dir_d, *new_dir_d;
	struct inode *old_dir_i, *new_dir_i;
	struct dentry *old_d, *new_d, *unlink_d;
	int error;
	int retval = 0;
	uint64_t now;

	nd_o->intent = LOOKUP_ACCESS; /* maybe, might need another type */

	/* get the parent, but don't follow links */
	error = path_lookup(old_path, LOOKUP_PARENT | LOOKUP_DIRECTORY, nd_o);
	if (error) {
		set_errno(-error);
		retval = -1;
		goto out_old_path;
	}
	old_dir_d = nd_o->dentry;
	old_dir_i = old_dir_d->d_inode;

	old_d = do_lookup(old_dir_d, nd_o->last.name);
	if (!old_d) {
		set_errno(ENOENT);
		retval = -1;
		goto out_old_path;
	}

	nd_n->intent = LOOKUP_CREATE;
	error = path_lookup(new_path, LOOKUP_PARENT | LOOKUP_DIRECTORY, nd_n);
	if (error) {
		set_errno(-error);
		retval = -1;
		goto out_paths_and_src;
	}
	new_dir_d = nd_n->dentry;
	new_dir_i = new_dir_d->d_inode;
	/* TODO if new_dir == old_dir, we might be able to simplify things */

	if (new_dir_i->i_sb != old_dir_i->i_sb) {
		set_errno(EXDEV);
		retval = -1;
		goto out_paths_and_src;
	}
	/* TODO: check_perms is lousy, want to just say "writable" here */
	if (check_perms(old_dir_i, S_IWUSR) || check_perms(new_dir_i, S_IWUSR)) {
		set_errno(EPERM);
		retval = -1;
		goto out_paths_and_src;
	}
	/* TODO: if we're doing a rename that moves a directory, we need to make
	 * sure the new_path doesn't include the old_path.  it's not as simple as
	 * just checking, since there could be a concurrent rename that breaks the
	 * check later.  e.g. what if new_dir's parent is being moved into a child
	 * of old_dir?
	 *
	 * linux has a per-fs rename mutex for these scenarios, so only one can
	 * proceed at a time.  i don't see another way to deal with it either.
	 * maybe something like flagging all dentries on the new_path with "do not
	 * move". */

	/* TODO: this is all very racy.  right after we do a new_d lookup, someone
	 * else could create or unlink new_d.  need to lock here, or else push this
	 * into the sub-FS.
	 *
	 * For any locking scheme, we probably need to lock both the old and new
	 * dirs.  To prevent deadlock, we need a total ordering of all inodes (or
	 * dentries, if we locking them instead).  inode number or struct inode*
	 * will work for this. */
	new_d = do_lookup(new_dir_d, nd_n->last.name);
	if (new_d) {
		if (new_d->d_inode == old_d->d_inode)
			goto out_paths_and_refs; 	/* rename does nothing */
		/* TODO: Here's a bunch of other racy checks we need to do, maybe in the
		 * sub-FS:
		 *
		 * if src is a dir, dst must be an empty dir if it exists (RACYx2)
		 * 		racing on dst being created and it getting new entries
		 * if src is a file, dst must be a file if it exists (RACY)
		 *		racing on dst being created and still being a file
		 *		racing on dst being unlinked and a new one being added
		 */
		/* TODO: we should allow empty dirs */
		if (S_ISDIR(new_d->d_inode->i_mode)) {
			set_errno(EISDIR);
			retval = -1;
			goto out_paths_and_refs;
		}
		/* TODO: need this to be atomic with rename */
		error = new_dir_i->i_op->unlink(new_dir_i, new_d);
		if (error) {
			set_errno(-error);
			retval = -1;
			goto out_paths_and_refs;
		}
		new_d->d_flags |= DENTRY_DYING;
		/* TODO: racy with other lookups on new_d */
		dcache_remove(new_d->d_sb, new_d);
		new_d->d_inode->i_nlink--;  /* TODO: race here, esp with a decref */
		kref_put(&new_d->d_kref);
	}
	/* new_d is just a vessel for the name.  somewhat lousy. */
	new_d = get_dentry(new_dir_d->d_sb, new_dir_d, nd_n->last.name);

	/* TODO: more races.  need to remove old_d from the dcache, since we're
	 * about to change its parentage.  could be readded concurrently. */
	dcache_remove(old_dir_d->d_sb, old_d);
	error = new_dir_i->i_op->rename(old_dir_i, old_d, new_dir_i, new_d);
	if (error) {
		/* TODO: oh crap, we already unlinked!  now we're screwed, and violated
		 * our atomicity requirements. */
		printk("[kernel] rename failed, you might have lost data\n");
		set_errno(-error);
		retval = -1;
		goto out_paths_and_refs;
	}

	/* old_dir loses old_d, new_dir gains old_d, renamed to new_d.  this is
	 * particularly cumbersome since there are two levels here: the FS has its
	 * info about where things are, and the VFS has its dentry tree.  and it's
	 * all racy (TODO). */
	dentry_set_name(old_d, new_d->d_name.name);
	old_d->d_parent = new_d->d_parent;
	if (S_ISDIR(old_d->d_inode->i_mode)) {
		TAILQ_REMOVE(&old_dir_d->d_subdirs, old_d, d_subdirs_link);
		old_dir_i->i_nlink--; /* TODO: racy, etc */
		TAILQ_INSERT_TAIL(&new_dir_d->d_subdirs, old_d, d_subdirs_link);
		new_dir_i->i_nlink--; /* TODO: racy, etc */
	}

	/* and then the third level: dcache stuff.  we could have old versions of
	 * old_d or negative versions of new_d sitting around.  dcache_put should
	 * replace a potentially negative dentry for new_d (now called old_d) */
	dcache_put(old_dir_d->d_sb, old_d);

	/* TODO could have a helper for this, but it's going away soon */
	now = epoch_seconds();
	old_dir_i->i_ctime.tv_sec = now;
	old_dir_i->i_mtime.tv_sec = now;
	old_dir_i->i_ctime.tv_nsec = 0;
	old_dir_i->i_mtime.tv_nsec = 0;
	new_dir_i->i_ctime.tv_sec = now;
	new_dir_i->i_mtime.tv_sec = now;
	new_dir_i->i_ctime.tv_nsec = 0;
	new_dir_i->i_mtime.tv_nsec = 0;

	/* fall-through */
out_paths_and_refs:
	kref_put(&new_d->d_kref);
out_paths_and_src:
	kref_put(&old_d->d_kref);
out_paths:
	path_release(nd_n);
out_old_path:
	path_release(nd_o);
	return retval;
}

int do_truncate(struct inode *inode, off64_t len)
{
	off64_t old_len;
	uint64_t now;
	if (len < 0) {
		set_errno(EINVAL);
		return -1;
	}
	if (len > PiB) {
		printk("[kernel] truncate for > petabyte, probably a bug\n");
		/* continuing, not too concerned.  could set EINVAL or EFBIG */
	}
	spin_lock(&inode->i_lock);
	old_len = inode->i_size;
	if (old_len == len) {
		spin_unlock(&inode->i_lock);
		return 0;
	}
	inode->i_size = len;
	/* truncate can't block, since we're holding the spinlock.  but it can rely
	 * on that lock being held */
	inode->i_op->truncate(inode);
	spin_unlock(&inode->i_lock);

	if (old_len < len) {
		pm_remove_contig(inode->i_mapping, old_len >> PGSHIFT,
		                 (len >> PGSHIFT) - (old_len >> PGSHIFT));
	}
	now = epoch_seconds();
	inode->i_ctime.tv_sec = now;
	inode->i_mtime.tv_sec = now;
	inode->i_ctime.tv_nsec = 0;
	inode->i_mtime.tv_nsec = 0;
	return 0;
}

struct file *alloc_file(void)
{
	struct file *file = kmem_cache_alloc(file_kcache, 0);
	if (!file) {
		set_errno(ENOMEM);
		return 0;
	}
	/* one for the ref passed out*/
	kref_init(&file->f_kref, file_release, 1);
	return file;
}

/* Opens and returns the file specified by dentry */
struct file *dentry_open(struct dentry *dentry, int flags)
{
	struct inode *inode;
	struct file *file;
	int desired_mode;
	inode = dentry->d_inode;
	/* Do the mode first, since we can still error out.  f_mode stores how the
	 * OS file is open, which can be more restrictive than the i_mode */
	switch (flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
		case O_RDONLY:
			desired_mode = S_IRUSR;
			break;
		case O_WRONLY:
			desired_mode = S_IWUSR;
			break;
		case O_RDWR:
			desired_mode = S_IRUSR | S_IWUSR;
			break;
		default:
			goto error_access;
	}
	if (check_perms(inode, desired_mode))
		goto error_access;
	file = alloc_file();
	if (!file)
		return 0;
	file->f_mode = desired_mode;
	/* Add to the list of all files of this SB */
	TAILQ_INSERT_TAIL(&inode->i_sb->s_files, file, f_list);
	kref_get(&dentry->d_kref, 1);
	file->f_dentry = dentry;
	kref_get(&inode->i_sb->s_mount->mnt_kref, 1);
	file->f_vfsmnt = inode->i_sb->s_mount;		/* saving a ref to the vmnt...*/
	file->f_op = inode->i_fop;
	/* Don't store creation flags */
	file->f_flags = flags & ~O_CREAT_FLAGS;
	file->f_pos = 0;
	file->f_uid = inode->i_uid;
	file->f_gid = inode->i_gid;
	file->f_error = 0;
//	struct event_poll_tailq		f_ep_links;
	spinlock_init(&file->f_ep_lock);
	file->f_privdata = 0;						/* prob overriden by the fs */
	file->f_mapping = inode->i_mapping;
	file->f_op->open(inode, file);
	return file;
error_access:
	set_errno(EACCES);
	return 0;
}

/* Closes a file, fsync, whatever else is necessary.  Called when the kref hits
 * 0.  Note that the file is not refcounted on the s_files list, nor is the
 * f_mapping refcounted (it is pinned by the i_mapping). */
void file_release(struct kref *kref)
{
	struct file *file = container_of(kref, struct file, f_kref);

	struct super_block *sb = file->f_dentry->d_sb;
	spin_lock(&sb->s_lock);
	TAILQ_REMOVE(&sb->s_files, file, f_list);
	spin_unlock(&sb->s_lock);

	/* TODO: fsync (BLK).  also, we may want to parallelize the blocking that
	 * could happen in here (spawn kernel threads)... */
	file->f_op->release(file->f_dentry->d_inode, file);
	/* Clean up the other refs we hold */
	kref_put(&file->f_dentry->d_kref);
	kref_put(&file->f_vfsmnt->mnt_kref);
	kmem_cache_free(file_kcache, file);
}

/* Process-related File management functions */

/* Given any FD, get the appropriate file, 0 o/w */
struct file *get_file_from_fd(struct files_struct *open_files, int file_desc)
{
	struct file *retval = 0;
	if (file_desc < 0)
		return 0;
	spin_lock(&open_files->lock);
	if (open_files->closed) {
		spin_unlock(&open_files->lock);
		return 0;
	}
	if (file_desc < open_files->max_fdset) {
		if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc)) {
			/* while max_files and max_fdset might not line up, we should never
			 * have a valid fdset higher than files */
			assert(file_desc < open_files->max_files);
			retval = open_files->fd[file_desc].fd_file;
			/* 9ns might be using this one, in which case file == 0 */
			if (retval)
				kref_get(&retval->f_kref, 1);
		}
	}
	spin_unlock(&open_files->lock);
	return retval;
}

/* Grow the vfs fd set */
static int grow_fd_set(struct files_struct *open_files) {
	int n;
	struct file_desc *nfd, *ofd;

	/* Only update open_fds once. If currently pointing to open_fds_init, then
 	 * update it to point to a newly allocated fd_set with space for
 	 * NR_FILE_DESC_MAX */
	if (open_files->open_fds == (struct fd_set*)&open_files->open_fds_init) {
		open_files->open_fds = kzmalloc(sizeof(struct fd_set), 0);
		memmove(open_files->open_fds, &open_files->open_fds_init,
		        sizeof(struct small_fd_set));
	}

	/* Grow the open_files->fd array in increments of NR_OPEN_FILES_DEFAULT */
	n = open_files->max_files + NR_OPEN_FILES_DEFAULT;
	if (n > NR_FILE_DESC_MAX)
		n = NR_FILE_DESC_MAX;
	nfd = kzmalloc(n * sizeof(struct file_desc), 0);
	if (nfd == NULL)
		return -1;

	/* Move the old array on top of the new one */
	ofd = open_files->fd;
	memmove(nfd, ofd, open_files->max_files * sizeof(struct file_desc));

	/* Update the array and the maxes for both max_files and max_fdset */
	open_files->fd = nfd;
	open_files->max_files = n;
	open_files->max_fdset = n;

	/* Only free the old one if it wasn't pointing to open_files->fd_array */
	if (ofd != open_files->fd_array)
		kfree(ofd);
	return 0;
}

/* Free the vfs fd set if necessary */
static void free_fd_set(struct files_struct *open_files) {
	if (open_files->open_fds != (struct fd_set*)&open_files->open_fds_init) {
		kfree(open_files->open_fds);
		assert(open_files->fd != open_files->fd_array);
		kfree(open_files->fd);
	}
}

/* 9ns: puts back an FD from the VFS-FD-space. */
int put_fd(struct files_struct *open_files, int file_desc)
{
	if (file_desc < 0) {
		warn("Negative FD!\n");
		return 0;
	}
	spin_lock(&open_files->lock);
	if (file_desc < open_files->max_fdset) {
		if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc)) {
			/* while max_files and max_fdset might not line up, we should never
			 * have a valid fdset higher than files */
			assert(file_desc < open_files->max_files);
			CLR_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc);
		}
	}
	spin_unlock(&open_files->lock);
	return 0;
}

/* Remove FD from the open files, if it was there, and return f.  Currently,
 * this decref's f, so the return value is not consumable or even usable.  This
 * hasn't been thought through yet. */
struct file *put_file_from_fd(struct files_struct *open_files, int file_desc)
{
	struct file *file = 0;
	if (file_desc < 0)
		return 0;
	spin_lock(&open_files->lock);
	if (file_desc < open_files->max_fdset) {
		if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc)) {
			/* while max_files and max_fdset might not line up, we should never
			 * have a valid fdset higher than files */
			assert(file_desc < open_files->max_files);
			file = open_files->fd[file_desc].fd_file;
			open_files->fd[file_desc].fd_file = 0;
			assert(file);	/* 9ns shouldn't call this put */
			kref_put(&file->f_kref);
			CLR_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc);
		}
	}
	spin_unlock(&open_files->lock);
	return file;
}

static int __get_fd(struct files_struct *open_files, int low_fd)
{
	int slot = -1;
	if ((low_fd < 0) || (low_fd > NR_FILE_DESC_MAX))
		return -EINVAL;
	if (open_files->closed)
		return -EINVAL;	/* won't matter, they are dying */

	/* Loop until we have a valid slot (we grow the fd_array at the bottom of
 	 * the loop if we haven't found a slot in the current array */
	while (slot == -1) {
		for (low_fd; low_fd < open_files->max_fdset; low_fd++) {
			if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, low_fd))
				continue;
			slot = low_fd;
			SET_BITMASK_BIT(open_files->open_fds->fds_bits, slot);
			assert(slot < open_files->max_files &&
			       open_files->fd[slot].fd_file == 0);
			if (slot >= open_files->next_fd)
				open_files->next_fd = slot + 1;
			break;
		}
		if (slot == -1)	{
			/* Expand the FD array and fd_set */
			if (grow_fd_set(open_files) == -1)
				return -ENOMEM;
			/* loop after growing */
		}
	}
	return slot;
}

/* Gets and claims a free FD, used by 9ns.  < 0 == error. */
int get_fd(struct files_struct *open_files, int low_fd)
{
	int slot;
	spin_lock(&open_files->lock);
	slot = __get_fd(open_files, low_fd);
	spin_unlock(&open_files->lock);
	return slot;
}

static int __claim_fd(struct files_struct *open_files, int file_desc)
{
	if ((file_desc < 0) || (file_desc > NR_FILE_DESC_MAX))
		return -EINVAL;
	if (open_files->closed)
		return -EINVAL;	/* won't matter, they are dying */

	/* Grow the open_files->fd_set until the file_desc can fit inside it */
	while(file_desc >= open_files->max_files) {
		grow_fd_set(open_files);
		cpu_relax();
	}

	/* If we haven't grown, this could be a problem, so check for it */
	if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc))
		return -ENFILE; /* Should never really happen. Here to catch bugs. */

	SET_BITMASK_BIT(open_files->open_fds->fds_bits, file_desc);
	assert(file_desc < open_files->max_files &&
	       open_files->fd[file_desc].fd_file == 0);
	if (file_desc >= open_files->next_fd)
		open_files->next_fd = file_desc + 1;
	return 0;
}

/* Claims a specific FD when duping FDs. used by 9ns.  < 0 == error. */
int claim_fd(struct files_struct *open_files, int file_desc)
{
	int ret;
	spin_lock(&open_files->lock);
	ret = __claim_fd(open_files, file_desc);
	spin_unlock(&open_files->lock);
	return ret;
}

/* Inserts the file in the files_struct, returning the corresponding new file
 * descriptor, or an error code.  We start looking for open fds from low_fd. */
int insert_file(struct files_struct *open_files, struct file *file, int low_fd,
                bool must)
{
	int slot, ret;
	spin_lock(&open_files->lock);
	if (must) {
		ret = __claim_fd(open_files, low_fd);
		if (ret < 0) {
			spin_unlock(&open_files->lock);
			return ret;
		}
		assert(!ret);	/* issues with claim_fd returning status, not the fd */
		slot = low_fd;
	} else {
		slot = __get_fd(open_files, low_fd);
	}

	if (slot < 0) {
		spin_unlock(&open_files->lock);
		return slot;
	}
	assert(slot < open_files->max_files &&
	       open_files->fd[slot].fd_file == 0);
	kref_get(&file->f_kref, 1);
	open_files->fd[slot].fd_file = file;
	open_files->fd[slot].fd_flags = 0;
	spin_unlock(&open_files->lock);
	return slot;
}

/* Closes all open files.  Mostly just a "put" for all files.  If cloexec, it
 * will only close files that are opened with O_CLOEXEC. */
void close_all_files(struct files_struct *open_files, bool cloexec)
{
	struct file *file;
	spin_lock(&open_files->lock);
	if (open_files->closed) {
		spin_unlock(&open_files->lock);
		return;
	}
	for (int i = 0; i < open_files->max_fdset; i++) {
		if (GET_BITMASK_BIT(open_files->open_fds->fds_bits, i)) {
			/* while max_files and max_fdset might not line up, we should never
			 * have a valid fdset higher than files */
			assert(i < open_files->max_files);
			file = open_files->fd[i].fd_file;
			/* no file == 9ns uses the FD.  they will deal with it */
			if (!file)
				continue;
			if (cloexec && !(open_files->fd[i].fd_flags & O_CLOEXEC))
				continue;
			/* Actually close the file */
			open_files->fd[i].fd_file = 0;
			assert(file);
			kref_put(&file->f_kref);
			CLR_BITMASK_BIT(open_files->open_fds->fds_bits, i);
		}
	}
	if (!cloexec) {
		free_fd_set(open_files);
		open_files->closed = TRUE;
	}
	spin_unlock(&open_files->lock);
}

/* Inserts all of the files from src into dst, used by sys_fork(). */
void clone_files(struct files_struct *src, struct files_struct *dst)
{
	struct file *file;
	spin_lock(&src->lock);
	if (src->closed) {
		spin_unlock(&src->lock);
		return;
	}
	spin_lock(&dst->lock);
	if (dst->closed) {
		warn("Destination closed before it opened");
		spin_unlock(&dst->lock);
		spin_unlock(&src->lock);
		return;
	}
	for (int i = 0; i < src->max_fdset; i++) {
		if (GET_BITMASK_BIT(src->open_fds->fds_bits, i)) {
			/* while max_files and max_fdset might not line up, we should never
			 * have a valid fdset higher than files */
			assert(i < src->max_files);
			file = src->fd[i].fd_file;
			assert(i < dst->max_files && dst->fd[i].fd_file == 0);
			SET_BITMASK_BIT(dst->open_fds->fds_bits, i);
			dst->fd[i].fd_file = file;
			/* no file means 9ns is using it, they clone separately */
			if (file)
				kref_get(&file->f_kref, 1);
			if (i >= dst->next_fd)
				dst->next_fd = i + 1;
		}
	}
	spin_unlock(&dst->lock);
	spin_unlock(&src->lock);
}

static void __chpwd(struct fs_struct *fs_env, struct dentry *new_pwd)
{
	struct dentry *old_pwd;
	kref_get(&new_pwd->d_kref, 1);
	/* writer lock, make sure we replace pwd with ours.  could also CAS.
	 * readers don't lock at all, so they need to either loop, or we need to
	 * delay releasing old_pwd til an RCU grace period. */
	spin_lock(&fs_env->lock);
	old_pwd = fs_env->pwd;
	fs_env->pwd = new_pwd;
	spin_unlock(&fs_env->lock);
	kref_put(&old_pwd->d_kref);
}

/* Change the working directory of the given fs env (one per process, at this
 * point).  Returns 0 for success, sets errno and returns -1 otherwise. */
int do_chdir(struct fs_struct *fs_env, char *path)
{
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	error = path_lookup(path, LOOKUP_DIRECTORY, nd);
	if (error) {
		set_errno(-error);
		path_release(nd);
		return -1;
	}
	/* nd->dentry is the place we want our PWD to be */
	__chpwd(fs_env, nd->dentry);
	path_release(nd);
	return 0;
}

int do_fchdir(struct fs_struct *fs_env, struct file *file)
{
	if ((file->f_dentry->d_inode->i_mode & __S_IFMT) != __S_IFDIR) {
		set_errno(ENOTDIR);
		return -1;
	}
	__chpwd(fs_env, file->f_dentry);
	return 0;
}

/* Returns a null-terminated string of up to length cwd_l containing the
 * absolute path of fs_env, (up to fs_env's root).  Be sure to kfree the char*
 * "kfree_this" when you are done with it.  We do this since it's easier to
 * build this string going backwards.  Note cwd_l is not a strlen, it's an
 * absolute size. */
char *do_getcwd(struct fs_struct *fs_env, char **kfree_this, size_t cwd_l)
{
	struct dentry *dentry = fs_env->pwd;
	size_t link_len;
	char *path_start, *kbuf;

	if (cwd_l < 2) {
		set_errno(ERANGE);
		return 0;
	}
	kbuf = kmalloc(cwd_l, 0);
	if (!kbuf) {
		set_errno(ENOMEM);
		return 0;
	}
	*kfree_this = kbuf;
	kbuf[cwd_l - 1] = '\0';
	kbuf[cwd_l - 2] = '/';
	/* for each dentry in the path, all the way back to the root of fs_env, we
	 * grab the dentry name, push path_start back enough, and write in the name,
	 * using /'s to terminate.  We skip the root, since we don't want it's
	 * actual name, just "/", which is set before each loop. */
	path_start = kbuf + cwd_l - 2;	/* the last byte written */
	while (dentry != fs_env->root) {
		link_len = dentry->d_name.len;		/* this does not count the \0 */
		if (path_start - (link_len + 2) < kbuf) {
			kfree(kbuf);
			set_errno(ERANGE);
			return 0;
		}
		path_start -= link_len;
		strncpy(path_start, dentry->d_name.name, link_len);
		path_start--;
		*path_start = '/';
		dentry = dentry->d_parent;	
	}
	return path_start;
}

static void print_dir(struct dentry *dentry, char *buf, int depth)
{
	struct dentry *child_d;
	struct dirent next = {0};
	struct file *dir;
	int retval;

	if (!S_ISDIR(dentry->d_inode->i_mode)) {
		warn("Thought this was only directories!!");
		return;
	}
	/* Print this dentry */
	printk("%s%s/ nlink: %d\n", buf, dentry->d_name.name,
	       dentry->d_inode->i_nlink);
	if (dentry->d_mount_point) {
		dentry = dentry->d_mounted_fs->mnt_root;
	}
	if (depth >= 32)
		return;
	/* Set buffer for our kids */
	buf[depth] = '\t';
	dir = dentry_open(dentry, 0);
	if (!dir)
		panic("Filesystem seems inconsistent - unable to open a dir!");
	/* Process every child, recursing on directories */
	while (1) {
		retval = dir->f_op->readdir(dir, &next);
		if (retval >= 0) {
			/* Skip .., ., and empty entries */
			if (!strcmp("..", next.d_name) || !strcmp(".", next.d_name) ||
			    next.d_ino == 0)
				goto loop_next;
			/* there is an entry, now get its dentry */
			child_d = do_lookup(dentry, next.d_name);
			if (!child_d)
				panic("Inconsistent FS, dirent doesn't have a dentry!");
			/* Recurse for directories, or just print the name for others */
			switch (child_d->d_inode->i_mode & __S_IFMT) {
				case (__S_IFDIR):
					print_dir(child_d, buf, depth + 1);
					break;
				case (__S_IFREG):
					printk("%s%s size(B): %d nlink: %d\n", buf, next.d_name,
					       child_d->d_inode->i_size, child_d->d_inode->i_nlink);
					break;
				case (__S_IFLNK):
					printk("%s%s -> %s\n", buf, next.d_name,
					       child_d->d_inode->i_op->readlink(child_d));
					break;
				case (__S_IFCHR):
					printk("%s%s (char device) nlink: %d\n", buf, next.d_name,
					       child_d->d_inode->i_nlink);
					break;
				case (__S_IFBLK):
					printk("%s%s (block device) nlink: %d\n", buf, next.d_name,
					       child_d->d_inode->i_nlink);
					break;
				default:
					warn("Look around you!  Unknown filetype!");
			}
			kref_put(&child_d->d_kref);	
		}
loop_next:
		if (retval <= 0)
			break;
	}
	/* Reset buffer to the way it was */
	buf[depth] = '\0';
	kref_put(&dir->f_kref);
}

/* Debugging */
int ls_dash_r(char *path)
{
	struct nameidata nd_r = {0}, *nd = &nd_r;
	int error;
	char buf[32] = {0};

	error = path_lookup(path, LOOKUP_ACCESS | LOOKUP_DIRECTORY, nd);
	if (error) {
		path_release(nd);
		return error;
	}
	print_dir(nd->dentry, buf, 0);
	path_release(nd);
	return 0;
}

/* Dummy ops, to catch weird operations we weren't expecting */
int dummy_create(struct inode *dir, struct dentry *dentry, int mode,
                 struct nameidata *nd)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

struct dentry *dummy_lookup(struct inode *dir, struct dentry *dentry,
                          struct nameidata *nd)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return 0;
}

int dummy_link(struct dentry *old_dentry, struct inode *dir,
             struct dentry *new_dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_unlink(struct inode *dir, struct dentry *dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_rmdir(struct inode *dir, struct dentry *dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_rename(struct inode *old_dir, struct dentry *old_dentry,
               struct inode *new_dir, struct dentry *new_dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

char *dummy_readlink(struct dentry *dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return 0;
}

void dummy_truncate(struct inode *inode)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
}

int dummy_permission(struct inode *inode, int mode, struct nameidata *nd)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_d_revalidate(struct dentry *dir, struct nameidata *nd)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_d_hash(struct dentry *dentry, struct qstr *name)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_d_compare(struct dentry *dir, struct qstr *name1, struct qstr *name2)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_d_delete(struct dentry *dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

int dummy_d_release(struct dentry *dentry)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
	return -1;
}

void dummy_d_iput(struct dentry *dentry, struct inode *inode)
{
	printk("Dummy VFS function %s called!\n", __FUNCTION__);
}

struct inode_operations dummy_i_op = {
	dummy_create,
	dummy_lookup,
	dummy_link,
	dummy_unlink,
	dummy_symlink,
	dummy_mkdir,
	dummy_rmdir,
	dummy_mknod,
	dummy_rename,
	dummy_readlink,
	dummy_truncate,
	dummy_permission,
};

struct dentry_operations dummy_d_op = {
	dummy_d_revalidate,
	dummy_d_hash,
	dummy_d_compare,
	dummy_d_delete,
	dummy_d_release,
	dummy_d_iput,
};
