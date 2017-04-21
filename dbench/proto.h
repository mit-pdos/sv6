#ifndef _PROTO_H_
#define _PROTO_H_

/* This file is automatically generated with "make proto". DO NOT EDIT */


/* The following definitions come from child.c  */

void child_run(struct child_struct *child0, const char *loadfile);

/* The following definitions come from dbench.c  */


/* The following definitions come from fileio.c  */


/* The following definitions come from io.c  */

void do_unlink(char *fname);
void expand_file(int fd, int size);
void do_open(char *fname, int handle, int size);
void do_write(int handle, int size, int offset);
void do_read(int handle, int size, int offset);
void do_close(int handle);
void do_mkdir(char *fname);
void do_rmdir(char *fname);
void do_rename(char *old, char *new);
void do_stat(char *fname, int size);
void do_create(char *fname, int size);

/* The following definitions come from libnfs.c  */

const char *nfs_error(int error);
void nfsio_disconnect(struct nfsio *nfsio);
struct nfsio *nfsio_connect(const char *server, const char *export, const char *protocol, int initial_xid, int xid_stride);

/* The following definitions come from mount_client.c  */


/* The following definitions come from mount_xdr.c  */


/* The following definitions come from nfs_client.c  */


/* The following definitions come from nfs_xdr.c  */


/* The following definitions come from nfsio.c  */


/* The following definitions come from snprintf.c  */


/* The following definitions come from sockio.c  */


/* The following definitions come from socklib.c  */

int open_socket_in(int type, int port);
int open_socket_out(const char *host, int port);
void set_socket_options(int fd, char *options);
int read_sock(int s, char *buf, int size);
int write_sock(int s, char *buf, int size);

/* The following definitions come from system.c  */

ssize_t sys_getxattr (const char *path, const char *name, void *value, size_t size);
ssize_t sys_fgetxattr (int filedes, const char *name, void *value, size_t size);
int sys_fsetxattr (int filedes, const char *name, const void *value, size_t size, int flags);

/* The following definitions come from tbench_srv.c  */


/* The following definitions come from util.c  */

void *shm_setup(int size);
void all_string_sub(char *s,const char *pattern,const char *insert);
void single_string_sub(char *s,const char *pattern,const char *insert);
BOOL next_token(char **ptr,char *buff,char *sep);
struct timeval timeval_current(void);
unsigned long timeval_elapsed(struct timeval *tv);
unsigned long timeval_elapsed2(struct timeval *tv1, struct timeval *tv2);
void msleep(unsigned int t);

#endif /*  _PROTO_H_  */
