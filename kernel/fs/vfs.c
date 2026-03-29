#include <types.h>
#include <param.h>
#include <macro.h>
#include <list.h>
#include <spinlock.h>
#include <proc.h>
#include <defs.h>
#include <file.h>
#include <fat32.h>
#include <fatfs.h>
#include <dirent.h>
#include <string.h>
#include <fs.h>
#include <vfs.h>
#include <console.h>

static struct file *find_file(struct list *list, int fd) {
  struct list_elem *e;
  for (e = list_begin(list); e != list_end(list); e = list_next(e)) {
    struct file *file = list_entry(e, struct file, elem);
    if (file->fd == fd)
      return file;
  }
  return NULL;
}

bool filesys_create(struct proc *p, char *path, mode_t mode) {
  struct dirent *base = p->cwd ? p->cwd->dirent : NULL;
  struct dirent *file = dirent_alloc();
  ASSERT_INFO(file != NULL, "alloc dirent fault");
  if (!file_create(base, path, mode, file)) {
    dirent_free(file);
    #ifdef CONFIG_DEBUG
    log("create file %s fault\n", path);
    #endif
    return false;
  }
  return true;
}

int filesys_open(struct proc *p, char *path, int flags) {
  struct dirent *dirent = NULL;
  enum file_type type = FD_FILE;
  #ifdef CONFIG_DEBUG
  log("filesys_open enter pid=%d path=%s\n", p ? p->pid : -1, path);
  #endif

  if (strncmp(path, "console", 7) == 0) {
    type = FD_DEVICE;
  } else {
    struct dirent *base = p->cwd != NULL ? p->cwd->dirent : NULL;
    #ifdef CONFIG_DEBUG
    log("filesys_open before file_open pid=%d path=%s\n", p ? p->pid : -1, path);
    #endif
    dirent = file_open(base, path, flags);
    #ifdef CONFIG_DEBUG
    log("filesys_open after file_open pid=%d path=%s dirent=%p\n", p ? p->pid : -1, path, dirent);
    #endif
    if (dirent == NULL) {
      return -1;
    }
  }

  struct file *file = kalloc(sizeof(struct file), FILE_MODE);
  file->flag = flags;
  file->dirent = dirent;
  file->fd = alloc_fd();
  file->pos = 0;
  file->deny_write = false;
  file->type = type;
  list_push_back(&p->file_list, &file->elem);
  #ifdef CONFIG_DEBUG
  log("filesys_open return pid=%d path=%s fd=%d\n", p ? p->pid : -1, path, file->fd);
  #endif
  return file->fd;
}

off_t filesys_write(struct proc *p, int fd, char *addr, size_t size) {
  struct file *file = find_file(&p->file_list, fd);
  if (file == NULL)
    return -1;
  if (file->type == FD_DEVICE) {
    return console_write(0, (uint64_t)addr, size);
  }
  size = file_write(file->dirent, addr, file->pos, size);
  file->pos += size;
  return size;
}

off_t filesys_read(struct proc *p, int fd, char *buf, size_t size) {
  struct file *file = find_file(&p->file_list, fd);
  if (file == NULL)
    return -1;
  if (file->type == FD_DEVICE) {
    return console_read(0, (uint64_t)buf, size);
  }
  size = file_read(file->dirent, buf, file->pos, size);
  file->pos += size;
  return size;
}

bool filesys_close(struct proc *p, int fd) {
  struct file *file = find_file(&p->file_list, fd);
  if (file == NULL)
    return true;
  else {
    struct dirent *dirent = file->dirent;
    free_fd(file->fd);
    list_remove(&file->elem);
    kfree(file, FILE_MODE);
    return file_close(dirent);
  }
}

static bool contian_of(struct file *file, char *name) {
  if (strncmp(file->dirent->name, name, strlen(name)) == 0)
    return true;
  else
    return false;
}
bool filesys_remove(struct proc *p, char *filename) {
  struct file *file = list_find(&p->file_list, struct file, contian_of, filename);
  bool ret = false;
  if (file != NULL) {
    struct dirent *dirent = file->dirent;
    free_fd(file->fd);
    list_remove(&file->elem);
    kfree(file, FILE_MODE);
    ret = file_remove(dirent, filename);
    if (!file_close(dirent))
      {
      #ifdef CONFIG_DEBUG
      log("Failure to close file before removing it\n");
      #endif
      }
  }
  return ret;
}

void filesys_seek(struct proc *p, int fd, off_t offset, int mode) {
  struct file *file = find_file(&p->file_list, fd);
  switch (mode) {
    case SEEK_SET:
      file->pos = offset;
      break;
    case SEEK_CUR:
      file->pos += offset;
      break;
    case SEEK_END:
      file->pos = file->dirent->size;
      break;
    default:
      #ifdef CONFIG_DEBUG
      log("invalid mode\n");
      #endif
      ASSERT(0);
  }
}

// bool filesys_link(struct proc *p, char *oldpath, char *newpath) {
//   struct dirent *old = file_open(p->cwd->dirent, oldpath, 0);
//   if (old == NULL)
//     return false;
//
//   struct dirent *file = dirent_alloc();
//
//   strncpy(file->name, newpath, strlen(newpath));
//   file->size = old->size;
//   file->first_cluster = old->first_cluster;
//   file->offset = old->offset;
//   file->type = DIR_LINK;
//   file->filesystem = old->filesystem;
//   file->linkcnt = 1;
//   file->mode = old->mode;
//   file->flag = flags;
//   old->linkcnt += 1;
//   return true;
// }
//
// struct file *filesys_deny_write(struct proc *p, int fd) {
//   struct file *file = find_file(&p->file_list, fd);
//   file->deny_write = true;
//   return file;
// }
//
// void  filesys_allow_write(struct file *entry) {
//   entry->deny_write = false;
// }
//
