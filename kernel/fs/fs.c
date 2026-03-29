#include <types.h>
#include <param.h>
#include <macro.h>
#include <stdio.h>
#include <list.h>
#include <fat32.h>
#include <fatfs.h>
#include <string.h>
#include <file.h>
#include <fs.h>
#include <spinlock.h>
#include <sleeplock.h>
#include <buf.h>
#include <pm.h>
#include <dirent.h>
#include <defs.h>

#define   MAX_FS_NUM    10
#define   FATDIR_PERM(entry) (*(char *)entry + SHORT_NAME_LEN)
#define   cal_cluster_pos(FstClusHI, FstClusLO) ((uint32_t)FstClusHI * (1 << 16) + (uint32_t)FstClusLO)
#define   is_long_file_name(entry)   ((entry->file_attribute & (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID))\
                                        == (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID))
static struct filesystem fs[MAX_FS_NUM];

// static bool is_equal_dirent(struct dirent *entry, const char *arg) {
//   if (strncmp(entry->name, arg, strlen(arg)) == 0) {
//     return true;
//   } else {
//     return false;
//   }
// }

// 读取簇内容
static void read_multiple_cluster(struct filesystem *fs, uint64_t clusNo, char *buf, uint32_t size) {
    for (uint32_t offset = 0; offset < size; offset += BSIZE) {
        clusread(fs, clusNo, offset, (uint64_t)(buf + offset), BSIZE);
    }
}

// 判断目录项是否无效
static int is_invalid_entry(struct FAT32Directory *entry) {
  if (entry->dir_name[0] == FATDIR_DLT ||
      strncmp((const char *)entry->dir_name, ".          ", SHORT_NAME_LEN) == 0 ||
      strncmp((const char *)entry->dir_name, "..         ", SHORT_NAME_LEN) == 0)
   return 0;
  else if (entry->dir_name[0] == 0)
    return -1;
  else
    return 1;
}

// 处理长文件名
static int handle_long_file_name(char *buf, uint32_t offset, int longEntSeq, char *rawName, int *aidx) {
  struct FAT32LongDirectory *longEnt = (struct FAT32LongDirectory *)(buf + offset);
  int prevSeq = longEntSeq;
  longEntSeq = FATLDIR_SEQ(*(char *)longEnt);
  int idx = *aidx; 

  if (prevSeq)    rawName[idx + 13] = '\0';
  wstr2str(rawName + idx, (char *)longEnt + 1, 10);
  wstr2str(rawName + 5 + idx, (char *)longEnt + 14, 12);
  wstr2str(rawName + 11 + idx, (char *)longEnt + 28, 4);
  *aidx = idx + 13;

  return longEntSeq;
}

// 填写文件名
static void fill_file_name(char *name, struct FAT32Directory *entry) {
    if (is_long_file_name(entry)) {
        fill_fat32_long_name(name, (char *)entry);
    } else {
        read_fat32_short_name(name, (char *)entry->dir_name);
    }
}

// 创建目录项
static bool match_dirent_entry(const char *name, struct dirent *parent, struct FAT32Directory *entry, uint32_t offset,
                        struct dirent **lastDir, const char *tname) {
  struct dirent *newDir = *lastDir;
  char _name[MAX_FILE_NAME_LEN];

  if (tname[0] != '\0') {
    strncpy(_name, tname, strlen(tname));
  } else {
    fill_file_name(_name, entry);
  }
  #ifdef CONFIG_DEBUG
  log("file name: %s\n", _name);
  #endif
  if (strncmp(_name, name, strlen(name)))
    return false;

  newDir->filesystem = parent->filesystem;
  newDir->first_cluster = cal_cluster_pos(entry->dir_FstClusHI, entry->dir_FstClusLO);
  newDir->size = entry->dir_FileSize;
  newDir->mode = entry->file_attribute;
  newDir->parent = parent;
  newDir->linkcnt = 1;
  newDir->parent->offset = offset;
  newDir->type = newDir->mode & ATTR_DIRECTORY ? DIR_DIR : DIR_FILE;
  if (newDir->mode & ATTR_DIRECTORY)
    list_init(&newDir->child);

  memcpy(newDir->name, name, strlen(name));

  return true;
}

// 解析簇中的目录项
static int parse_directory_entries(const char *name, struct dirent *parent, char *buf, uint32_t clusSize, struct dirent **lastDir) {
  bool longEntSeq = false;
  char tname[MAX_FILE_NAME_LEN];
  int idx = 0;

  memset(tname, 0, MAX_FILE_NAME_LEN);
  for (uint32_t i = 0; i < clusSize; i += DIRECTORY_SIZE) {
    struct FAT32Directory *entry = (struct FAT32Directory *)(buf + i);

    int ret = is_invalid_entry(entry);
    if (ret == 0) {
      idx = 0;
      memset(tname, 0, MAX_FILE_NAME_LEN);
      longEntSeq = false;
      continue;
    } else if (ret == -1)
      return -1;

    if (is_long_file_name(entry)) {
      longEntSeq = handle_long_file_name(buf, i, longEntSeq, tname, &idx);
      continue;
    }

    if (match_dirent_entry(name, parent, entry, i, lastDir, tname))
      return 0;

    idx = 0;
    memset(tname, 0, MAX_FILE_NAME_LEN);
  }
  return -E_NOT_FOUND;
}

static void find_dirent_entry(struct dirent *dir, const char *name, struct dirent **entry) {
  uint64_t clusNo = dir->first_cluster;
  struct filesystem *fs = dir->filesystem;
  uint32_t clusSize = fs->sbinfo.bytes_per_clus;
  char *buf = kpmalloc();

  while (1) {
    read_multiple_cluster(fs, clusNo, buf, clusSize);
    int result = parse_directory_entries(name, dir, buf, clusSize, entry);

    if (result == -1) {
      break;  // 到达文件尾或无效簇
    }
    if (result == 0) {
      #ifdef CONFIG_DEBUG
      log("find %s\n", name);
      #endif
      break;
    }

    clusNo = fatread(fs, clusNo);  // 获取下一个簇号
    if (FAT32_isEOF(clusNo)) {
      break;  // 到达文件尾
    }
  }
  kpmfree(buf);
}

void init_fs(void) {
  for (int i = 0; i < MAX_FS_NUM; i++) {
    fs[i].valid = false;
  }
}
struct filesystem *alloc_fs() {
  for (int i = 0; i < MAX_FS_NUM; i++) {
    if (fs[i].valid == false) {
      memset((void *)&fs[i], 0, sizeof(struct filesystem));
      fs[i].valid = true;
      return &fs[i];
    }
  }
  panic("no more fs to alloc");
}

static char *skip_slash(char *path) {
  while (*path == '/')
    ++path;
  return path;
}

static int parse_path(const char *path, char token[MAX_FILE_NAME_LEN][MAX_FILE_NAME_LEN]) {
  if (path == NULL) return -1;  // 检查空指针
  
  char *p = (char *)path;
  p = skip_slash(p);  // 跳过初始的 '/'
  int i = 0;

  while (i < MAX_FILE_NAME_LEN) {  // 检查数组边界
    if (strlen(p) == 0)
      break;
    char *tmp = p;
    while (*tmp != '/' && *tmp != '\0')  // 修正条件
      ++tmp;

    int len = tmp - p;
    if (len >= MAX_FILE_NAME_LEN) len = MAX_FILE_NAME_LEN - 1;  // 防止单个部分过长

    strncpy(token[i], p, len);  // 复制子字符串
    token[i][len] = '\0';  // 确保字符串以 '\0' 结尾
    ++i;

    p = skip_slash(tmp);  // 跳过下一个 '/'
  }

  return i;  // 返回分割的部分数
}

static int walkDir(struct filesystem *fs, char *path, struct dirent *base_dir, struct dirent **dir,
                   struct dirent **file, char *lastElem, struct long_entry_set *longSet) {
  char token[MAX_FILE_NAME_LEN][MAX_FILE_NAME_LEN];
  int token_size = parse_path(path, token);

  struct dirent *cfile = dirent_alloc();
  struct dirent *cdir = base_dir == NULL ? fs->root : base_dir;

  for (int i = 0 ; i < token_size; i++) {
    struct dirent *tmp = NULL;
    // if (cdir->type == DIR_DIR)
    //   tmp = list_find(&cdir->child, struct dirent, is_equal_dirent, token[i]);
    if (tmp == NULL)
      find_dirent_entry(cdir, token[i], &cfile);
    else
      memcpy(cfile, tmp, sizeof(struct dirent));

    if (cfile->type == DIR_DIR) {
      cdir = cfile;
    } else if (cfile->type == DIR_FILE) {
      if (i == token_size - 1) {
        if (lastElem)
          strncpy(lastElem, token[i], strlen(token[i]));
        memcpy(*file, cfile, sizeof (struct dirent) );
        dirent_free(cfile);
        return 0;
      }
      if (lastElem)
        strncpy(lastElem, token[i], strlen(token[i]));
      dirent_free(cfile);
      return -E_NOT_FOUND;
    } else {
      // link not implement
    }
  }
  dirent_free(cfile);
  return -E_NOT_FOUND;
}

bool createItemAt(struct dirent *base_dir, const char *path, struct dirent **file, mode_t mode, bool is_dir) {
  struct filesystem *fs;
	char lastElem[MAX_FILE_NAME_LEN];
	struct dirent *dir, *f;
	int r;
	struct long_entry_set longSet;

  if (base_dir) {
    fs = base_dir->filesystem;
  } else {
    fs = get_root_fs();
  }

  char cpath[strlen(path)+1];
  strncpy(cpath, path, strlen(path));
  if ((r = walkDir(fs, cpath, base_dir, &dir, &f, lastElem, &longSet)) == 0) {
    #ifdef CONFIG_DEBUG
    log("file already exist\n");
    #endif
    return false;
  }
  panic("createItemAt todo");
  return false;
}

struct dirent *get_file(struct dirent *base_dir, const char *path) {
  struct dirent *dirent = dirent_alloc();
  struct filesystem *fs;
  struct long_entry_set longSet;

  fs = base_dir == NULL ? get_root_fs() : base_dir->filesystem;

  int r = walkDir(fs, (char *)path, base_dir, 0, &dirent, 0, &longSet);
  if (r < 0)
    return NULL;
  else
    return dirent;
}

static uint32_t file_get_clusNo(struct dirent *file, uint32_t index) {
  uint32_t clus = file->first_cluster;
  for (int i = 0; i < index; i++) {
    clus = fatread(file->filesystem, clus);
    if (FAT32_isEOF(clus))
      break;
  }
  return clus;
}

/**
  * file: 需要读取的文件
  * dst:  从文件中读取的数据的目的地址
  * off:  文件的偏移量
  * n:    需要读取的字节数
  **/
int fileread(struct dirent *file, uint64_t dst, uint32_t off, uint32_t n) {
  if (off > file->size) {
    return -E_EXCEED_FILE;
  } else if ((off + n) > file->size) {
    n = file->size - off;
  }

  if (n == 0)   return 0;

  uint64_t start = off;
  uint64_t end   = off + n - 1;
  uint32_t clus_size = CLUSTER_SIZE(file->filesystem);
  uint32_t offset = off % clus_size;
  uint32_t len = 0;
  uint32_t clus = file_get_clusNo(file, start / clus_size);
  if (FAT32_isEOF(clus))
    return -1;

  clusread(file->filesystem, clus, offset, dst, MIN(n, clus_size - offset));
  len += MIN(n, clus_size - offset);

  clus = fatread(file->filesystem, clus);
  uint32_t clus_num = (end - start) / clus_size;
  for (int i = 0; i < clus_num; i++) {
    clusread(file->filesystem, clus, 0, dst + len, MIN(n - len, clus_size));
    clus = fatread(file->filesystem, clus);
    len += MIN(n - len, clus_size);
  }

  return len;
}

/**
  * file: 需要扩展的文件
  * new_size: 新的文件大小
  **/
static void fileExtend(struct dirent *file, uint32_t new_size) {
  if (file->size > new_size)  return;

  uint32_t clus_size = CLUSTER_SIZE(file->filesystem);
  uint32_t extend_clus = (new_size - file->size) / clus_size +
                          ((new_size - file->size) % clus_size == 0 ? 0 : 1);
  file->size = new_size;
  uint32_t clus = file->first_cluster;
  uint32_t last_clus = 0;
  for (; !FAT32_isEOF(clus);) {
    last_clus = clus;
    clus = fatread(file->filesystem, clus);
  }

  for (int i = 0; i < extend_clus; i++) {
    last_clus = clusalloc(file->filesystem, last_clus);
  }
}

int filewrite(struct dirent *file, uint64_t src, uint32_t off, uint32_t n) {
  if (n == 0)   return 0;

  if (off > file->size) {
    return -E_EXCEED_FILE;
  } else if (off + n > file->size) {
    fileExtend(file, off + n);
  }

  uint64_t start = off;
  uint64_t end   = off + n - 1;
  uint32_t clus_num = ROUNDUP(end-start, CLUSTER_SIZE(fs)) / CLUSTER_SIZE(fs);
  uint32_t offset = off % CLUSTER_SIZE(fs);
  uint64_t clus = file_get_clusNo(file, start / CLUSTER_SIZE(fs));
  if (FAT32_isEOF(clus))
    return -1;
  uint32_t len = 0;

  cluswrite(file->filesystem, clus, offset, src, MIN(n, CLUSTER_SIZE(fs) - offset));
  len += MIN(n, CLUSTER_SIZE(fs) - offset);
  clus = fatread(file->filesystem, clus);

  for (int i = 1; i < clus_num; i++) {
    cluswrite(file->filesystem, clus, offset, src, MIN(n - len, CLUSTER_SIZE(fs)));
    len += MIN(n - len, CLUSTER_SIZE(fs));
    clus = fatread(file->filesystem, clus);
  }

  return len;
}

char buf[8192];
void fat32Test() {
	// 测试读取文件
	struct dirent *file = get_file(NULL, "/text.txt");
	ASSERT(file);
	ASSERT(fileread(file, (uint64_t)buf, 0, file->size) >= 0);
	printf("%s\n", buf);

	// 测试写入文件
	char *str = "Hello! I\'m "
		    "rl!"
		    "\n3233333333233333333233333333233333333233333333233333333233333333233333333233"
		    "333333233333333233333333233333333233333333233333333233333333233333333233333333"
		    "233333333233333333222222222233233333333233333333233333333233333333233333333233"
		    "333333233333333233333333233333333233333333233333333233333333233333333233333333"
		    "233333333233333333222222222233233333333233333333233333333233333333233333333233"
		    "333333233333333233333333233333333233333333233333333233333333233333333233333333"
		    "233333333233333333222222222233233333333233333333233333333233333333233333333233"
		    "333333233333333233333333233333333233333333233333333233333333233333333233333333"
		    "23333333323333333322222222222222222222222222\n This is end!";
	int len = strlen(str) + 1;
	ASSERT(filewrite(file, (uint64_t)str, 0, len) >= 0);

	// 读出文件
	ASSERT(fileread(file, (uint64_t)buf, 0, file->size) >= 0);
  ASSERT(strncmp(buf, str, strlen(str)) == 0);
	#ifdef CONFIG_DEBUG
	log("FAT32 Test Passed!\n");
	#endif
}
