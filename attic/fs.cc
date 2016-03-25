
// Unused code, but potentially useful (at least as a reference).

u64
ino_hash(const pair<u32, u32> &p)
{
  return p.first ^ p.second;
}

static nstbl<pair<u32, u32>, inode*, ino_hash> *ins;

template<size_t N>
struct inode_cache;

template<size_t N>
struct inode_cache : public balance_pool<inode_cache<N>>
{
  inode_cache()
    : balance_pool<inode_cache<N>> (N),
      head_(0), length_(0), lock_("inode_cache", LOCKSTAT_FS)
  {
  }

  int
  alloc()
  {
    scoped_acquire _l(&lock_);
    return alloc_nolock();
  }

  void
  add(u32 inum)
  {
    scoped_acquire _l(&lock_);
    add_nolock(inum);
  }

  void
  balance_move_to(inode_cache<N>* target)
  {
    if (target < this) {
      target->lock_.acquire();
      lock_.acquire();
    } else {
      lock_.acquire();
      target->lock_.acquire();
    }

    u32 nmove = length_ / 2;
    for (; nmove; nmove--) {
      int inum = alloc_nolock();
      if (inum < 0) {
        console.println("inode_cache: unexpected failure");
        break;
      }
      target->add_nolock(inum);
    }

    if (target < this) {
      target->lock_.release();
      lock_.release();
    } else {
      lock_.release();
      target->lock_.release();
    }
  }

  u64
  balance_count() const
  {
    return length_;
  }

private:

  int
  alloc_nolock()
  {
    int inum = -1;
    if (length_) {
      length_--;
      head_--;
      inum = cache_[head_ % N];
    }
    return inum;
  }

  void
  add_nolock(u32 inum)
  {
    assert(inum != 0);
    if (length_ < N)
      length_++;
    cache_[head_ % N] = inum;
    head_++;
  }

  u32      cache_[N];
  u32      head_;
  u32      length_;
  spinlock lock_;
};

struct inode_cache_dir
{
  inode_cache_dir() : balancer_(this)
  {
  }

  inode_cache<512>*
  balance_get(int id) const
  {
    return &cache_[id];
  }

  void
  add(u32 inum)
  {
    // XXX(sbw) if cache->length_ == N should we call
    // balancer_.balance()?
    cache_->add(inum);
  }

  int
  alloc()
  {
    int inum = cache_->alloc();
    if (inum > 0)
      return inum;
    balancer_.balance();
    return cache_->alloc();
  }

private:

  percpu<inode_cache<512>, NO_CRITICAL> cache_;
  balancer<inode_cache_dir, inode_cache<512>> balancer_;
};

static inode_cache_dir the_inode_cache;

void
dir_flush(sref<inode> dp, transaction *trans)
{
  // assume already locked
  //cprintf("Calling dir_flush on dp with inum %d\n", dp->inum);
  if (!dp->dir)
    return;

  u32 off = 0;
  char *buffer = (char *)zalloc("dir_flush");

  dp->dir.load()->enumerate([&dp, &off, trans, buffer](const strbuf<DIRSIZ> &name, const u32 &inum)->bool{
      struct dirent de;
      strncpy(de.name, name.buf_, DIRSIZ);
      de.inum = inum;

      void *buf = buffer + off;
      const void *de_ptr = &de;
      memmove(buf, de_ptr, sizeof(de));
      off += sizeof(de);

      if (off > PGSIZE)
        panic("dir_flush buffer overflow");

      return false;
    });

  if (writei(dp, buffer, 0, PGSIZE, trans) != PGSIZE)
    panic("dir_flush writei");

  if (dp->size != off) {
    auto w = dp->seq.write_begin();
    dp->size = off;
  }
  iupdate(dp, trans);
}

void
dir_remove_entries(sref<inode> dp, std::vector<char*> names_vec) {
  dir_init(dp);
  dp->dir.load()->enumerate([&names_vec, &dp](const strbuf<DIRSIZ> &name, const u32 &inum)->bool{
      bool exists = false;
      for (auto it = names_vec.begin(); it != names_vec.end(); it++) {
        if (strcmp(*it, name.buf_) == 0) {
          exists = true;
          break;
        }
      }
      if (exists) {
        sref<inode> ip = iget(dp->dev, inum);
        if (ip->type == T_DIR)
          dirunlink(dp, name.buf_, inum, true);
        else if (ip->type == T_FILE)
          dirunlink(dp, name.buf_, inum, false);
      }
      return false;
    });
}

void
dir_remove_entry(sref<inode> dp, char* entry_name) {
  dir_init(dp);
  dp->dir.load()->enumerate([&entry_name, &dp](const strbuf<DIRSIZ> &name, const u32 &inum)->bool{
      if (strcmp(entry_name, name.buf_) == 0) {
        sref<inode> ip = iget(dp->dev, inum);
        if (ip->type == T_DIR)
          dirunlink(dp, name.buf_, inum, true);
        else if (ip->type == T_FILE)
          dirunlink(dp, name.buf_, inum, false);
      }
      return false;
    });
}