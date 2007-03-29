#include "redir.h"

static kmem_cache_t *rinode_cache = NULL;
unsigned long long rinode_cnt = 0;
spinlock_t rinode_cnt_lock = SPIN_LOCK_UNLOCKED;
extern atomic_t rinodes_freed;
extern wait_queue_head_t rinodes_wait;

struct rinode *rinode_alloc(struct inode *inode)
{
	struct rinode *rinode = NULL;


	rinode = kmem_cache_alloc(rinode_cache, SLAB_KERNEL);
	if (!rinode)
		return ERR_PTR(RFS_ERR_NOMEM);

	INIT_RCU_HEAD(&rinode->ri_rcu);
	INIT_LIST_HEAD(&rinode->ri_rdentries);
	rinode->ri_inode = inode;
	rinode->ri_op_old = inode->i_op;
	rinode->ri_fop_old = (struct file_operations *)inode->i_fop;
	rinode->ri_aop_old = (struct address_space_operations *)inode->i_mapping->a_ops;
	rinode->ri_path = NULL;
	rinode->ri_chain = NULL;
	rinode->ri_path_set = NULL;
	rinode->ri_chain_set = NULL;
	rinode->ri_ops_set = NULL;
	atomic_set(&rinode->ri_count, 1);
	atomic_set(&rinode->ri_nlink, 1);
	spin_lock_init(&rinode->ri_lock);

	if (inode->i_op)
		memcpy(&rinode->ri_op_new, inode->i_op,
				sizeof(struct inode_operations));
	else
		memset(&rinode->ri_op_new, 0,
				sizeof(struct inode_operations));

	if (inode->i_mapping->a_ops)
		memcpy(&rinode->ri_aop_new, inode->i_mapping->a_ops,
				sizeof(struct address_space_operations));
	else
		memset(&rinode->ri_aop_new, 0,
				sizeof(struct address_space_operations));

	spin_lock(&rinode_cnt_lock);
	rinode_cnt++;
	spin_unlock(&rinode_cnt_lock);

	return rinode;
}

inline struct rinode *rinode_get(struct rinode *rinode)
{
	BUG_ON(!atomic_read(&rinode->ri_count));
	atomic_inc(&rinode->ri_count);
	return rinode;
}

inline void rinode_put(struct rinode *rinode)
{
	if (!rinode || IS_ERR(rinode))
		return;

	BUG_ON(!atomic_read(&rinode->ri_count));
	if (!atomic_dec_and_test(&rinode->ri_count))
		return;
	
	path_put(rinode->ri_path);
	chain_put(rinode->ri_chain);
	path_put(rinode->ri_path_set);
	chain_put(rinode->ri_chain_set);
	ops_put(rinode->ri_ops_set);
	BUG_ON(!list_empty(&rinode->ri_rdentries));
	kmem_cache_free(rinode_cache, rinode);

	spin_lock(&rinode_cnt_lock);
	if (!--rinode_cnt)
		atomic_set(&rinodes_freed, 1);
	spin_unlock(&rinode_cnt_lock);

	if (atomic_read(&rinodes_freed))
		wake_up_interruptible(&rinodes_wait);
}

inline struct rinode *rinode_find(struct inode *inode)
{
	struct rinode *rinode = NULL;
	struct inode_operations *i_op;


	rcu_read_lock();
	i_op = rcu_dereference(inode->i_op);
	if (i_op) {
		if (i_op->lookup == rfs_lookup) {
			rinode = container_of(i_op, struct rinode, ri_op_new);
			rinode = rinode_get(rinode);
		}
	}
	rcu_read_unlock();

	return rinode;
}

static inline void rinode_del_rcu(struct rcu_head *head)
{
	struct rinode *rinode = NULL;

	
	rinode = container_of(head, struct rinode, ri_rcu);
	rinode_put(rinode);
}

void rinode_del(struct inode *inode)
{
	struct rinode *rinode = NULL;


	spin_lock(&inode->i_lock);

	rinode = rinode_find(inode);
	if (!rinode) {
		spin_unlock(&inode->i_lock);
		return;
	}

	if (!atomic_dec_and_test(&rinode->ri_nlink)) {
		spin_unlock(&inode->i_lock);
		rinode_put(rinode);
		return;
	}

	inode->i_fop = rinode->ri_fop_old;
	inode->i_mapping->a_ops = rinode->ri_aop_old;
	rcu_assign_pointer(inode->i_op, rinode->ri_op_old);

	rinode_put(rinode);

	spin_unlock(&inode->i_lock);

	call_rcu(&rinode->ri_rcu, rinode_del_rcu);
}

int rfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct rinode *parent = NULL;
	struct path *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct path *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->mkdir)
			 return dir->i_op->mkdir(dir, dentry, mode);
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_mkdir.dir = dir;
	args.args.i_mkdir.dentry = dentry;
	args.args.i_mkdir.mode = mode;
	args.type.id = RFS_DIR_IOP_MKDIR;

	if (!rfs_precall_flts(chain, NULL, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->mkdir)
			rv = parent->ri_op_old->mkdir(dir, dentry, mode);

		args.retv.rv_int = rv;

		if (!rfs_postcall_flts(chain, NULL, &args))
			rv = args.retv.rv_int;
	} else 
		rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	return rv;
}

int rfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	struct rinode *parent = NULL;
	struct path *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct path *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->create)
			return dir->i_op->create(dir, dentry, mode, nd);
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_create.dir = dir;
	args.args.i_create.dentry = dentry;
	args.args.i_create.mode = mode;
	args.args.i_create.nd = nd;
	args.type.id = RFS_DIR_IOP_CREATE;

	if (!rfs_precall_flts(chain, NULL, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->create)
			rv = parent->ri_op_old->create(dir, dentry, mode, nd);

		args.retv.rv_int = rv;

		if (!rfs_postcall_flts(chain, NULL, &args))
			rv = args.retv.rv_int;
	} else 
		rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	return rv;
}

struct dentry *rfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	struct rinode *parent = NULL;
	struct path *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct path *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct dentry *rv = NULL;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->lookup)
			return dir->i_op->lookup(dir, dentry, nd);
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_lookup.dir = dir;
	args.args.i_lookup.dentry = dentry;
	args.args.i_lookup.nd = nd;
	args.type.id = RFS_DIR_IOP_LOOKUP;

	if (!rfs_precall_flts(chain, NULL, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->lookup)
			rv = parent->ri_op_old->lookup(dir, dentry, nd);

		args.retv.rv_dentry = rv;

		if (!rfs_postcall_flts(chain, NULL, &args))
			rv = args.retv.rv_dentry;
	} else
		rv = args.retv.rv_dentry;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	return rv;
}

int rfs_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	struct rinode *rinode = NULL;
	struct path *path = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	int submask = mask & ~MAY_APPEND;
	int rv = 0;

	rinode = rinode_find(inode);
	if (!rinode) {
		if (inode->i_op && inode->i_op->permission)
			return inode->i_op->permission(inode, mask, nd);
		else
			return generic_permission(inode, submask, NULL);
	}

	spin_lock(&rinode->ri_lock);
	path = path_get(rinode->ri_path);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.i_permission.inode = inode;
	args.args.i_permission.mask = mask;
	args.args.i_permission.nd = nd;
	args.type.id = RFS_DIR_IOP_PERMISSION;

	if (!rfs_precall_flts(chain, NULL, &args)) {
		if (rinode->ri_op_old && rinode->ri_op_old->permission)
			rv = rinode->ri_op_old->permission(inode, mask, nd);
		else
			rv = generic_permission(inode, submask, NULL);

		args.retv.rv_int = rv;

		if (!rfs_postcall_flts(chain, NULL, &args))
			rv = args.retv.rv_int;

	} else
		rv = args.retv.rv_int;

	rinode_put(rinode);
	path_put(path);
	chain_put(chain);

	return rv;
}

void rinode_set_reg_ops(struct rinode *rinode, int *ops)
{
	if (ops[RFS_REG_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;
}

void rinode_set_dir_ops(struct rinode *rinode, int *ops)
{
	if (ops[RFS_DIR_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	rinode->ri_op_new.mkdir = rfs_mkdir;
	rinode->ri_op_new.create = rfs_create;
}

void rinode_set_ops(struct rinode *rinode, struct ops *ops)
{
	umode_t mode = rinode->ri_inode->i_mode;

	if (S_ISREG(mode))
		rinode_set_reg_ops(rinode, ops->o_ops);

	else if (S_ISDIR(mode))
		rinode_set_dir_ops(rinode, ops->o_ops);

	rinode->ri_op_new.lookup = rfs_lookup;
}

int rinode_cache_create(void)
{
	rinode_cache = kmem_cache_create("rinode_cache",
					  sizeof(struct rinode),
					  0, SLAB_RECLAIM_ACCOUNT,
					  NULL, NULL);
	if (!rinode_cache)
		return -1;

	return 0;
}

void rinode_cache_destroy(void)
{
	kmem_cache_destroy(rinode_cache);
}

