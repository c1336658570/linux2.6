/*
 * include/linux/buffer_head.h
 *
 * Everything to do with buffer_heads.
 */

#ifndef _LINUX_BUFFER_HEAD_H
#define _LINUX_BUFFER_HEAD_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/wait.h>
#include <asm/atomic.h>

#ifdef CONFIG_BLOCK

enum bh_state_bits {
	// 该缓冲区包含可用数据
	BH_Uptodate,	/* Contains valid data */
	// 该缓冲区是脏的
	BH_Dirty,	/* Is dirty */
	// 该缓冲区正在被I/O操作使用，被锁定以防止并发访问
	BH_Lock,	/* Is locked */
	// 该缓冲区有I/O请求操作
	BH_Req,		/* Has been submitted for I/O */
	// 用于页面中的第一个缓冲区，以串行化页面中其他缓冲区的 I/O 完成
	BH_Uptodate_Lock,/* Used by the first bh in a page, to serialise
			  * IO completion of other buffers in the page
			  */

	// 该缓冲区是映射磁盘块的可用缓冲区
	BH_Mapped,	/* Has a disk mapping */
	// 该缓冲区是通过get_block刚刚映射的，尚且不能访问
	BH_New,		/* Disk mapping was newly created by get_block */
	// 该缓冲区正在通过end_buffer_async_read被异步I/O读操作使用
	BH_Async_Read,	/* Is under end_buffer_async_read I/O */
	// 该缓冲区正通过end_buffer_async_write被异步I/O写操作使用
	BH_Async_Write,	/* Is under end_buffer_async_write I/O */
	// 该缓冲区尚未和磁盘块相关联
	BH_Delay,	/* Buffer is not yet allocated on disk */
	// 该缓冲区处于连续块区边界，下一个块不再连续
	BH_Boundary,	/* Block is followed by a discontiguity */
	// 该缓冲区在写的时候遇到I/O错误
	BH_Write_EIO,	/* I/O error on write */
	// 顺序写
	BH_Ordered,	/* ordered write */
	// 该缓冲区发生不被支持的错误
	BH_Eopnotsupp,	/* operation not supported (barrier) */
	// 该缓冲区在硬盘上的空间已被申请，但是没有实际的数据写出
	BH_Unwritten,	/* Buffer is allocated on disk but not written */
	// 此缓冲区禁止错误
	BH_Quiet,	/* Buffer Error Prinks to be quiet */

	// 不是可用状态标志位，使用它是为了指明可被其他代码使用的起始位。块I/O层不会使用该标志位或更高的位。
	// 驱动程序可以在这些位中定义自己的状态标志，只要保证不与块I/O层专用位发生冲突就行。
	BH_PrivateStart,/* not a state bit, but the first bit available
			 * for private allocation by other entities
			 */
};

#define MAX_BUF_PER_PAGE (PAGE_CACHE_SIZE / 512)

struct page;
struct buffer_head;
struct address_space;
typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);

/*
 * Historically, a buffer_head was used to map a single block
 * within a page, and of course as the unit of I/O through the
 * filesystem and block layers.  Nowadays the basic I/O unit
 * is the bio, and buffer_heads are used for extracting block
 * mappings (via a get_block_t call), for tracking state within
 * a page (via a page_mapping) and for wrapping bio submission
 * for backward compatibility reasons (e.g. submit_bh).
 */
/*
 * 结构体 buffer_head 在 Linux 内核中定义，用于管理文件系统的块映射和缓冲区头。
 * 历史上，buffer_head 用于映射页面内的单个块，并作为通过文件系统和块层的 I/O 单元。
 * 现在，基本的 I/O 单位是 bio，buffer_head 主要用于：
 * 1. 通过 get_block_t 调用提取块映射，
 * 2. 通过 page_mapping 追踪页面内的状态，
 * 3. 为向后兼容而封装 bio 提交（例如 submit_bh）。
 */
// 缓冲区头的目的在于描述磁盘块和物理内存缓冲区（在特定页面上的字节序列）之间的映射关系。
// 该结构只有一个作用，说明缓冲区到块的映射关系
struct buffer_head {
	// 缓冲区的状态标志，在bh_state_bits的enum中
	unsigned long b_state;		/* buffer state bitmap (see above) */
	// 页面中的缓冲区
	// b_this_page 字段链接了同一物理页的所有缓冲区头，形成一个循环链表。
	// 因为一个页4K，如果一个块1K，那么一页就可以有4个缓冲区，b_this_page将这4个缓冲区连在一起
	struct buffer_head *b_this_page;/* circular list of page's buffers */
	// 存储缓冲区的页面（与缓冲区对应的内存物理页）
	struct page *b_page;		/* the page this bh is mapped to */

	// 起始块号，是b_bdev域所对应的设备的逻辑块号
	sector_t b_blocknr;		/* start block number */
	// 映像的大小
	size_t b_size;			/* size of mapping */
	// 页面内的数据指针，直接指向相应的块（它位于b_page域所指明的页面中的某个位置上）
	// 块大小是b_size，所以块在内存中的起始位置是b_data，结束位置是b_data+b_size
	/*
	 * buffer_head中的b_data指向对应的缓冲区地址。注意：如果page是high mem,b_data
	 * 存放的缓冲区业内的偏移量，比如第一个缓冲区b_data = 0，第二个是1K，第三个是2K。
	 * 如果page在非high mem，b_data指向对应缓冲区的虚拟地址。
	 */
	char *b_data;			/* pointer to data within the page */
	
	// 相关联的块设备
	struct block_device *b_bdev;
	// I/O完成方法
	bh_end_io_t *b_end_io;		/* I/O completion */
	// 为 b_end_io 保留的私有数据
 	void *b_private;		/* reserved for b_end_io */
	// 相关的映射链表
	struct list_head b_assoc_buffers; /* associated with another mapping */
	// 相关的地址空间
	struct address_space *b_assoc_map;	/* mapping this buffer is
						   associated with */
	// 缓冲区使用计数，通过get_bh和put_bh增加或减少引用计数。
	atomic_t b_count;		/* users using this buffer_head */
};

/*
 * macro tricks to expand the set_buffer_foo(), clear_buffer_foo()
 * and buffer_foo() functions.
 */
#define BUFFER_FNS(bit, name)						\
static inline void set_buffer_##name(struct buffer_head *bh)		\
{									\
	set_bit(BH_##bit, &(bh)->b_state);				\
}									\
static inline void clear_buffer_##name(struct buffer_head *bh)		\
{									\
	clear_bit(BH_##bit, &(bh)->b_state);				\
}									\
static inline int buffer_##name(const struct buffer_head *bh)		\
{									\
	return test_bit(BH_##bit, &(bh)->b_state);			\
}

/*
 * test_set_buffer_foo() and test_clear_buffer_foo()
 */
#define TAS_BUFFER_FNS(bit, name)					\
static inline int test_set_buffer_##name(struct buffer_head *bh)	\
{									\
	return test_and_set_bit(BH_##bit, &(bh)->b_state);		\
}									\
static inline int test_clear_buffer_##name(struct buffer_head *bh)	\
{									\
	return test_and_clear_bit(BH_##bit, &(bh)->b_state);		\
}									\

/*
 * Emit the buffer bitops functions.   Note that there are also functions
 * of the form "mark_buffer_foo()".  These are higher-level functions which
 * do something in addition to setting a b_state bit.
 */
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
TAS_BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Ordered, ordered)
BUFFER_FNS(Eopnotsupp, eopnotsupp)
BUFFER_FNS(Unwritten, unwritten)

#define bh_offset(bh)		((unsigned long)(bh)->b_data & ~PAGE_MASK)
#define touch_buffer(bh)	mark_page_accessed(bh->b_page)

/* If we *know* page->private refers to buffer_heads */
#define page_buffers(page)					\
	({							\
		BUG_ON(!PagePrivate(page));			\
		((struct buffer_head *)page_private(page));	\
	})
#define page_has_buffers(page)	PagePrivate(page)

/*
 * Declarations
 */

void mark_buffer_dirty(struct buffer_head *bh);
void init_buffer(struct buffer_head *, bh_end_io_t *, void *);
void set_bh_page(struct buffer_head *bh,
		struct page *page, unsigned long offset);
int try_to_free_buffers(struct page *);
struct buffer_head *alloc_page_buffers(struct page *page, unsigned long size,
		int retry);
void create_empty_buffers(struct page *, unsigned long,
			unsigned long b_state);
void end_buffer_read_sync(struct buffer_head *bh, int uptodate);
void end_buffer_write_sync(struct buffer_head *bh, int uptodate);
void end_buffer_async_write(struct buffer_head *bh, int uptodate);

/* Things to do with buffers at mapping->private_list */
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode);
int inode_has_buffers(struct inode *);
void invalidate_inode_buffers(struct inode *);
int remove_inode_buffers(struct inode *inode);
int sync_mapping_buffers(struct address_space *mapping);
void unmap_underlying_metadata(struct block_device *bdev, sector_t block);

void mark_buffer_async_write(struct buffer_head *bh);
void __wait_on_buffer(struct buffer_head *);
wait_queue_head_t *bh_waitq_head(struct buffer_head *bh);
struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block,
			unsigned size);
struct buffer_head *__getblk(struct block_device *bdev, sector_t block,
			unsigned size);
void __brelse(struct buffer_head *);
void __bforget(struct buffer_head *);
void __breadahead(struct block_device *, sector_t block, unsigned int size);
struct buffer_head *__bread(struct block_device *, sector_t block, unsigned size);
void invalidate_bh_lrus(void);
struct buffer_head *alloc_buffer_head(gfp_t gfp_flags);
void free_buffer_head(struct buffer_head * bh);
void unlock_buffer(struct buffer_head *bh);
void __lock_buffer(struct buffer_head *bh);
void ll_rw_block(int, int, struct buffer_head * bh[]);
int sync_dirty_buffer(struct buffer_head *bh);
int submit_bh(int, struct buffer_head *);
void write_boundary_block(struct block_device *bdev,
			sector_t bblock, unsigned blocksize);
int bh_uptodate_or_lock(struct buffer_head *bh);
int bh_submit_read(struct buffer_head *bh);

extern int buffer_heads_over_limit;

/*
 * Generic address_space_operations implementations for buffer_head-backed
 * address_spaces.
 */
void block_invalidatepage(struct page *page, unsigned long offset);
int block_write_full_page(struct page *page, get_block_t *get_block,
				struct writeback_control *wbc);
int block_write_full_page_endio(struct page *page, get_block_t *get_block,
			struct writeback_control *wbc, bh_end_io_t *handler);
int block_read_full_page(struct page*, get_block_t*);
int block_is_partially_uptodate(struct page *page, read_descriptor_t *desc,
				unsigned long from);
int block_write_begin(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page **, void **, get_block_t*);
int block_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
int generic_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
void page_zero_new_buffers(struct page *page, unsigned from, unsigned to);
int block_prepare_write(struct page*, unsigned, unsigned, get_block_t*);
int cont_write_begin(struct file *, struct address_space *, loff_t,
			unsigned, unsigned, struct page **, void **,
			get_block_t *, loff_t *);
int generic_cont_expand_simple(struct inode *inode, loff_t size);
int block_commit_write(struct page *page, unsigned from, unsigned to);
int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf,
				get_block_t get_block);
void block_sync_page(struct page *);
sector_t generic_block_bmap(struct address_space *, sector_t, get_block_t *);
int block_truncate_page(struct address_space *, loff_t, get_block_t *);
int file_fsync(struct file *, struct dentry *, int);
int nobh_write_begin(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page **, void **, get_block_t*);
int nobh_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
int nobh_truncate_page(struct address_space *, loff_t, get_block_t *);
int nobh_writepage(struct page *page, get_block_t *get_block,
                        struct writeback_control *wbc);

void buffer_init(void);

/*
 * inline definitions
 */

static inline void attach_page_buffers(struct page *page,
		struct buffer_head *head)
{
	page_cache_get(page);
	SetPagePrivate(page);
	set_page_private(page, (unsigned long)head);
}

// 缓冲区使用计数，通过get_bh增加引用计数。
static inline void get_bh(struct buffer_head *bh)
{
        atomic_inc(&bh->b_count);
}

// 缓冲区使用计数，通过put_bh减少引用计数。
static inline void put_bh(struct buffer_head *bh)
{
        smp_mb__before_atomic_dec();
        atomic_dec(&bh->b_count);
}

static inline void brelse(struct buffer_head *bh)
{
	if (bh)
		__brelse(bh);
}

static inline void bforget(struct buffer_head *bh)
{
	if (bh)
		__bforget(bh);
}

/**
 * sb_bread() - 从指定的文件系统超级块读取一个块，并返回包含该块的缓冲区头
 * @sb: 指向文件系统超级块的指针
 * @block: 要读取的块号
 * 
 * 该函数封装了 __bread 函数，通过给定的超级块信息和块号，读取相应的块。
 * 它使用超级块所在的块设备和块大小来调用 __bread 函数。
 */
// 参数为超级块和块号
static inline struct buffer_head *
sb_bread(struct super_block *sb, sector_t block)
{
	// 调用 __bread 函数，传入超级块的块设备、块号和块大小
	return __bread(sb->s_bdev, block, sb->s_blocksize);
}

static inline void
sb_breadahead(struct super_block *sb, sector_t block)
{
	__breadahead(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *
sb_getblk(struct super_block *sb, sector_t block)
{
	return __getblk(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *
sb_find_get_block(struct super_block *sb, sector_t block)
{
	return __find_get_block(sb->s_bdev, block, sb->s_blocksize);
}

static inline void
map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block)
{
	set_buffer_mapped(bh);
	bh->b_bdev = sb->s_bdev;
	bh->b_blocknr = block;
	bh->b_size = sb->s_blocksize;
}

/*
 * Calling wait_on_buffer() for a zero-ref buffer is illegal, so we call into
 * __wait_on_buffer() just to trip a debug check.  Because debug code in inline
 * functions is bloaty.
 */
static inline void wait_on_buffer(struct buffer_head *bh)
{
	might_sleep();
	if (buffer_locked(bh) || atomic_read(&bh->b_count) == 0)
		__wait_on_buffer(bh);
}

static inline int trylock_buffer(struct buffer_head *bh)
{
	return likely(!test_and_set_bit_lock(BH_Lock, &bh->b_state));
}

static inline void lock_buffer(struct buffer_head *bh)
{
	might_sleep();
	if (!trylock_buffer(bh))
		__lock_buffer(bh);
}

extern int __set_page_dirty_buffers(struct page *page);

#else /* CONFIG_BLOCK */

static inline void buffer_init(void) {}
static inline int try_to_free_buffers(struct page *page) { return 1; }
static inline int inode_has_buffers(struct inode *inode) { return 0; }
static inline void invalidate_inode_buffers(struct inode *inode) {}
static inline int remove_inode_buffers(struct inode *inode) { return 1; }
static inline int sync_mapping_buffers(struct address_space *mapping) { return 0; }

#endif /* CONFIG_BLOCK */
#endif /* _LINUX_BUFFER_HEAD_H */
