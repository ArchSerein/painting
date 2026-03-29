#ifndef VFS_H__
#define VFS_H__

typedef uint32_t  mode_t;

enum file_type { FD_NONE, FD_FILE, FD_DEVICE };

struct file {
  uint32_t          fd;
  struct dirent *   dirent;
  off_t             pos;
  bool              deny_write;
  mode_t            flag;
  struct list_elem  elem;
  enum file_type    type;
};

bool      filesys_create(struct proc *, char *, mode_t);
int       filesys_open(struct proc *, char *, int);
bool      filesys_close(struct proc *, int);
off_t     filesys_read(struct proc *, int, char *, size_t);
off_t     filesys_write(struct proc *, int, char *, size_t);
bool      filesys_remove(struct proc *, char *);
void      filesys_seek(struct proc *, int, off_t, int);
bool      filesys_link(struct proc *, char *, char *);
void      filesys_unlink(struct proc *p, char *);

/*struct file *filesys_deny_write(struct proc *, int);*/
/*void         filesys_allow_write(struct file *);*/
#endif // !VFS_H__
