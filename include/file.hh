#pragma once

#include "cpputil.hh"
#include "ns.hh"
#include "gc.hh"
#include <atomic>
#include "refcache.hh"
#include "eager_refcache.hh"
#include "condvar.hh"
#include "semaphore.hh"
#include "mfs.hh"
#include "sleeplock.hh"
#include <uk/unistd.h>

class dir_entries;

struct file {
  virtual int fsync() { return -1; }
  // Duplicate this file so it can be bound to a FD.
  virtual file* dup() { inc(); return this; }

  // Called when a FD using this file is being closed, just before
  // dec().  This can be an explicit close(), or the termination of a
  // process.  This will always be paired with a dup().
  virtual void pre_close() { }

  virtual int stat(struct stat*, enum stat_flags) { return -1; }
  virtual ssize_t read(char *addr, size_t n) { return -1; }
  virtual ssize_t write(const char *addr, size_t n) { return -1; }
  virtual ssize_t pread(char *addr, size_t n, off_t offset) { return -1; }
  virtual ssize_t pwrite(const char *addr, size_t n, off_t offset) { return -1; }

  // Socket operations
  virtual int bind(const struct sockaddr *addr, size_t addrlen) { return -1; }
  virtual int listen(int backlog) { return -1; }
  // Unlike the syscall, the return is only an error status.  The
  // caller will allocate an FD for *out on success.  addrlen is only
  // an out-argument.
  virtual int accept(struct sockaddr_storage *addr, size_t *addrlen, file **out)
  { return -1; }
  // sendto and recvfrom take a userptr to the buf to avoid extra
  // copying in the kernel.  The other pointers will be kernel
  // pointers.  dest_addr may be null.
  virtual ssize_t sendto(userptr<void> buf, size_t len, int flags,
                         const struct sockaddr *dest_addr, size_t addrlen)
  { return -1; }
  // Unlike the syscall, addrlen is only an out-argument, since
  // src_addr will be big enough for any sockaddr.  src_addr may be
  // null.
  virtual ssize_t recvfrom(userptr<void> buf, size_t len, int flags,
                           struct sockaddr_storage *src_addr,
                           size_t *addrlen)
  { return -1; }

  virtual sref<mnode> get_mnode() { return sref<mnode>(); }

  virtual void inc() = 0;
  virtual void dec() = 0;

protected:
  file() {}
};

struct file_mnode : public refcache::referenced, public file {
public:
  file_mnode(sref<mnode> m, bool r, bool w, bool a)
    : m(m), readable(r), writable(w), append(a), off(0) {}
  NEW_DELETE_OPS(file_mnode);

  void inc() override { refcache::referenced::inc(); }
  void dec() override { refcache::referenced::dec(); }

  const sref<mnode> m;
  const bool readable;
  const bool writable;
  const bool append;
  u32 off;
  sleeplock off_lock;

  int fsync() override;
  int stat(struct stat*, enum stat_flags) override;
  ssize_t read(char *addr, size_t n) override;
  ssize_t write(const char *addr, size_t n) override;
  ssize_t pread(char* addr, size_t n, off_t off) override;
  ssize_t pwrite(const char *addr, size_t n, off_t offset) override;
  void onzero() override
  {
    delete this;
  }

  sref<mnode> get_mnode() override { return m; }
};

struct file_pipe_reader : public refcache::referenced, public file {
public:
  file_pipe_reader(pipe* p) : pipe(p) {}
  NEW_DELETE_OPS(file_pipe_reader);

  void inc() override { refcache::referenced::inc(); }
  void dec() override { refcache::referenced::dec(); }

  int stat(struct stat*, enum stat_flags) override;
  ssize_t read(char *addr, size_t n) override;
  void onzero() override;

private:
  struct pipe* const pipe;
};

// We need to detect immediately when there are no more pipe writers.
// To do this while avoiding sharing in the common case, we use a
// two-level approach to pipe writer reference counting.
//
//          pipe
//            ↑             (fixed reference)
//     file_pipe_writer
//    ↗       ↑        ↖    (eager references)
// wrapper wrapper wrapper
//    ↑       ↑     ↑  ↑    (hybrid references)
//   FD      FD    FD temp
//
// Each pipe has exactly one file_pipe_writer that represents its
// write end.  This is always eagerly reference counted and as soon as
// it reaches zero, the write end is closed.  However, this is not
// what an FD table entry points to.  Each FD table entry gets a
// unique file_pipe_writer_wrapper, which in turn references the
// file_pipe_writer.  Hence, the reference count on the
// file_pipe_writer is the number of FDs that are open to it.  No
// more, no less.
//
// file_pipe_writer_wrapper is hybrid counted.  As long as the FD is
// open, it has at least one reference, so it operates in scalable
// mode and thus temporary references are scalable.  When the FD is
// closed, it switches to eager mode, and as soon as the last
// reference to the wrapper is dropped, the wrapper will be destroyed
// and release its reference to the file_pipe_writer (potentially
// closing the pipe).

struct file_pipe_writer_wrapper : public eager_refcache::referenced, public file {
public:
  file_pipe_writer_wrapper(file* f) : inner(f) {}
  NEW_DELETE_OPS(file_pipe_writer_wrapper);

  void inc() override { referenced::inc(); }
  void dec() override { referenced::dec(); }

  file* dup() override {
    return inner->dup();
  }

  int stat(struct stat* st, enum stat_flags flags) override {
    return inner->stat(st, flags);
  }

  ssize_t write(const char *addr, size_t n) override {
    return inner->write(addr, n);
  }

  void pre_close() override {
    // This FD is being closed.  Now we need to know the moment its
    // reference count actually drops to zero so we can immediately
    // decrement the write end of the pipe.  (close()'s reference is
    // *probably* the last reference, but there may be concurrent
    // operations holding transient references on this FD.)
    eagerify();

    // XXX It's really hard to convince yourself that we never miss a
    // pre_close, especially in error-handling cases.  I'm pretty sure
    // it's true because we only get a file_pipe_writer_wrapper when
    // we dup a file_pipe_writer, and we only do that when we're about
    // to install it in the filetable, and if the filetable dup's a
    // struct file, it always pre_closes it.  We could make this
    // simpler by starting eager_refcache::referenced in *eager* mode
    // and only switching it to scalable mode when we "commit" the
    // reference.  I think the eager to scalable transition only
    // requires setting referenced::mode_.
  }

  void onzero() override {
    inner->dec();
    delete this;
  }

private:
  file* inner;
};

struct file_pipe_writer : public referenced, public file {
public:
  file_pipe_writer(pipe* p) : pipe(p) {}
  NEW_DELETE_OPS(file_pipe_writer);

  void inc() override { referenced::inc(); }
  void dec() override { referenced::dec(); }

  file* dup() override {
    inc();
    file* w = new file_pipe_writer_wrapper(this);
    return w ?: this;
  }

  int stat(struct stat*, enum stat_flags) override;
  ssize_t write(const char *addr, size_t n) override;
  void onzero() override;

private:
  struct pipe* const pipe;
};

// in-core file system types
struct inode : public referenced, public rcu_freed
{
  void  init();
  void  link();
  void  unlink();
  short nlink();

  inode& operator=(const inode&) = delete;
  inode(const inode& x) = delete;

  void do_gc() override { delete this; }

  // const for lifetime of object:
  const u32 dev;
  const u32 inum;

  // const unless inode is reused:
  u32 gen;
  std::atomic<short> type;
  short major;
  short minor;

  std::atomic<bool> valid;

  // locks for the rest of the inode
  struct condvar cv;
  struct spinlock lock;
  char lockname[16];

  // protected by the lock
  bool busy;
  int readbusy;

  u32 size;
  u32 addrs[NDIRECT+2];
  short nlink_;

  dir_entries* dir;
  u32 dir_offset; // The next dir-entry gets added at this offset.

  // ??? what's the concurrency control plan?
  struct localsock *localsock;
  char socketpath[PATH_MAX];

private:
  inode(u32 dev, u32 inum);
  ~inode();
  NEW_DELETE_OPS(inode)

  static sref<inode> alloc(u32 dev, u32 inum);
  friend void initinode_early();
  friend void initinode_late();
  friend sref<inode> iget(u32, u32);

protected:
  void onzero() override;
};

typedef struct free_inum {
  u32 inum_;
  int cpu;
  bool is_free;
  ilink<free_inum> link;

  free_inum(u32 inum, bool f): inum_(inum), is_free(f) {}
  NEW_DELETE_OPS(free_inum);

  free_inum& operator=(const free_inum&) = delete;
  free_inum(const free_inum&) = delete;

  free_inum& operator=(free_inum&&) = default;
  free_inum(free_inum&&) = default;
} free_inum;

// The freeinum_bitmap in memory, used to perform inode number allocations
// for on-disk inodes.
struct freeinum_bitmap {
  // We maintain the bitmap as both a vector and a linked-list so that we
  // can perform both allocations and frees in O(1) time. The inum_vector
  // contains entries for all inums, whereas the inum_freelist contains
  // entries only for inums that are actually free. The allocator consumes
  // items from the inum_freelist in O(1) time; the free code locates the
  // free_inum data-structure corresponding to the inum being freed in O(1)
  // time using the inum_vector and inserts it into the inum_freelist (also
  // in O(1) time). Items are never removed from the inum_vector so as to
  // enable the O(1) lookups.
  std::vector<free_inum*> inum_vector;

  struct freelist {
    ilist<free_inum, &free_inum::link> inum_freelist;
    spinlock list_lock; // Guards modifications to the inum_freelist.
  };

  // We maintain per-CPU freelists for scalability. The inum_vector is
  // read-only after initialization, so a single one will suffice.
  percpu<struct freelist> freelists;
  struct freelist reserve_freelist; // Global reserve pool of free inums.
};

// device implementations

class mdev;

struct devsw {
  int (*read)(mdev*, char*, u32);
  int (*pread)(mdev*, char*, u32, u32);
  int (*write)(mdev*, const char*, u32);
  int (*pwrite)(mdev*, const char*, u32, u32);
  void (*stat)(mdev*, struct stat*);
};

extern struct devsw devsw[];
