#include <types.h>
#include <param.h>
#include <spinlock.h>
#include <sleeplock.h>
#include <macro.h>
#include <fat32.h>
#include <list.h>
#include <fatfs.h>
#include <buf.h>
#include <string.h>
#include <defs.h>
#include <dirent.h>
#include <riscv.h>
#include <stdio.h>

static void cluszero(struct filesystem *, uint32_t);
// init
// fill in superblock
static int clusinit(struct filesystem *fs) {
  // 从设备中读取设备信息
  struct buf *buf = bread(fs->deviceNum, 0);
  if (buf == NULL)
    return ERR;

  struct fat32hder *header = (struct fat32hder *)(buf->data);
  fs->superblock.bytes_per_sector = header->bytes_per_sector;
  fs->superblock.sectors_per_cluster = header->sectors_per_cluster;
  fs->superblock.reserved_sectors = header->reserved_sectors;
  fs->superblock.cluster_root_directory = header->cluster_root_directory;
  fs->superblock.fat_size = header->fat_size;

  ASSERT_INFO(!strncmp((const char *)(header->system_identifier_string), "FAT32", 5), "read fat32 volume error");

  // 记录 superblock 的信息
  // RootDirsectors = 0
  fs->sbinfo.first_data_sector = header->reserved_sectors + 
                                    header->number_fats * header->fat_size;
  fs->sbinfo.data_sector_cnt = header->large_sector_count -
                                  fs->sbinfo.first_data_sector;
  fs->sbinfo.cluster_cnt = fs->sbinfo.data_sector_cnt /
                            header->sectors_per_cluster;
  fs->sbinfo.bytes_per_clus = header->bytes_per_sector *
                            header->sectors_per_cluster;

  if (BSIZE != header->bytes_per_sector)
    return ERR;
  brelse(buf);

  return 0;
}

/**
 * @brief 计数文件的簇数
 */
static int count_clusters(struct dirent *file) {
	#ifdef CONFIG_DEBUG
	log("count Cluster begin!\n");
	#endif

	int clus = file->first_cluster;
	int i = 0;
	if (clus == 0) {
		#ifdef CONFIG_DEBUG
		log("cluster is 0!\n");
		#endif
		return 0;
	}
	// 如果文件不包含任何块，则直接返回0即可。
	else {
		while (!FAT32_isEOF(clus)) {
			clus = fatread(file->filesystem, clus);
			i += 1;
		}
		#ifdef CONFIG_DEBUG
		log("count Cluster end!\n");
		#endif
		return i;
	}
}

void fat32_init(struct filesystem *fs) {
  #ifdef CONFIG_DEBUG
  log("init fat32 ...\n");
  #endif
  strncpy(fs->name, "FAT32", 5);

  ASSERT_INFO(clusinit(fs) == 0, "clusinit fault");
  #ifdef CONFIG_DEBUG
  log("clus init finish\n");
  #endif

  fs->root = dirent_alloc();
  fs->root->first_cluster = fs->superblock.cluster_root_directory;
  strncpy(fs->root->name, "/", 1);
  fs->root->filesystem = fs;
  fs->root->mode |= ATTR_DIRECTORY;
  fs->root->parent = NULL;
  fs->root->linkcnt = 1;
  fs->root->offset = 0;
  fs->root->size = count_clusters(fs->root) * CLUSTER_SIZE(fs);
  list_init(&fs->root->child);
  ASSERT_INFO(sizeof(struct FAT32Directory) == DIRENT_SIZE, "FAT32Directory size is not DIRENT_SIZE");

  #ifdef CONFIG_DEBUG
  log("fat32 fs init finish\n");
  #endif
}

uint32_t fatread(struct filesystem *fs, uint32_t clusterNo) {
  if (clusterNo < fs->superblock.cluster_root_directory ||
      clusterNo > fs->sbinfo.data_sector_cnt + 1)
    return 0;

  // zero -> current fat
  uint32_t sectorNo   = SectorNum(fs->superblock, clusterNo, 0);
  uint32_t sectorOff  = FATEntOffset(fs->superblock, clusterNo);

  struct buf *b = bread(fs->deviceNum, sectorNo);
  uint32_t *data = (uint32_t *)(b->data);
  uint32_t ret = data[sectorOff / 4];
  brelse(b);
  return ret;
}

void fatwrite(struct filesystem *fs, uint32_t clusterNo, uint32_t nextNo) {
  if (clusterNo < fs->superblock.cluster_root_directory ||
      clusterNo > fs->sbinfo.data_sector_cnt + 1) {
    ASSERT_INFO(0, "write fat");
    return;
  }

  uint32_t sectorNo0  =  SectorNum(fs->superblock, clusterNo, 0);
  uint32_t sectorNo1  =  SectorNum(fs->superblock, clusterNo, 1);
  uint32_t sectorOff  =  FATEntOffset(fs->superblock, clusterNo);

  struct buf *b = bread(fs->deviceNum, sectorNo0);
  uint32_t *data = (uint32_t *)(b->data);
  data[sectorOff / 4] = nextNo;
  bwrite(b);

  struct buf *b1 = bread(fs->deviceNum, sectorNo1);
  data = (uint32_t *)(b->data);
  data[sectorOff / 4] = nextNo;
  bwrite(b1);

  brelse(b);
  brelse(b1);
}

uint32_t clusalloc(struct filesystem *fs, uint32_t prevNo) {
  uint32_t nextNo = 0;

  // 从第一个簇开始遍历FAT表
  for (nextNo = 2; nextNo < fs->sbinfo.cluster_cnt + 2; nextNo++) {
    uint32_t fat_entry = fatread(fs, nextNo); // 读取FAT表中的当前簇状态
    
    // 检查当前簇是否空闲（FAT32_EOF表示簇为空闲）
    if (fat_entry == 0) {
      // 如果是空闲簇，更新FAT表，连接前一个簇
      if (prevNo != 0) {
        fatwrite(fs, prevNo, nextNo);  // 写入前一个簇的FAT表项
      }
      fatwrite(fs, nextNo, FAT32_EOF); // 标记当前簇为EOF
      cluszero(fs, nextNo);            // 清空簇
      return nextNo;
    }
  }

  // 如果没有找到空闲簇
  #ifdef CONFIG_DEBUG
  log("out of data cluster space");
  #endif
  return 0;
}

void clusfree(struct filesystem *fs, uint32_t clusNo, uint32_t prevNo) {
  if (prevNo != 0) {
    fatwrite(fs, prevNo, FAT32_EOF);
  }
  fatwrite(fs, clusNo, 0);
}

static void cluszero(struct filesystem *fs, uint32_t clusNo) {
  uint32_t sectorSize = fs->superblock.bytes_per_sector;
  uint32_t clusSector = fs->superblock.sectors_per_cluster;
  uint32_t idx = first_sector_clus(fs, clusNo);
  for (uint32_t i = 0; i < clusSector; idx++, i++) {
    struct buf *b = bread(fs->deviceNum, idx);
    memset(b->data, 0, sectorSize);
    bwrite(b);
    brelse(b);
  }
}

uint32_t clusread(struct filesystem *fs, uint32_t clusNo, uint32_t offset
                  ,uint64_t dst, uint64_t n) {
  // 找到需要读的第一个 sector
  uint32_t secNo = first_sector_clus(fs, clusNo) + offset /
                        fs->superblock.bytes_per_sector;
  uint32_t secOff = offset % fs->superblock.bytes_per_sector;
  ASSERT_INFO(n <= fs->sbinfo.bytes_per_clus - offset,
              "clusread: The size of the \
              required read exceeds the remaining space in the cluster.");
  uint32_t read_len = 0;
  uint32_t len = MIN(fs->superblock.bytes_per_sector - secOff, n);
  struct buf *b = bread(fs->deviceNum, secNo);
  memcpy((void *)dst + read_len, (void *)&b->data[secOff], len);
  brelse(b);
  if (len == n) return len;
  read_len += len;
  for (secNo = secNo + 1; read_len < n; secNo++) {
    len = MIN(fs->superblock.bytes_per_sector, n - read_len);
    b = bread(fs->deviceNum, secNo);
    memcpy((void *)dst + read_len, b->data, len);
    brelse(b);
    read_len += len;
  }
  return read_len;
}

uint32_t cluswrite(struct filesystem *fs, uint32_t clusNo, uint32_t offset
                   ,uint64_t src, uint64_t n) {
  uint32_t secNo = first_sector_clus(fs, clusNo) + offset /
                        fs->superblock.bytes_per_sector;
  uint32_t secOff = offset % fs->superblock.bytes_per_sector;
  ASSERT_INFO(n < fs->sbinfo.bytes_per_clus - offset,
              "cluswrite: The size of the \
              required write exceeds the remaining space in the cluster.");

  uint32_t write_len = 0;
  uint32_t len = MIN(fs->superblock.bytes_per_sector - secOff, n);
  struct buf *b = bread(fs->deviceNum, secNo);
  memcpy((void *)&b->data[secOff], (void *)src + write_len, len);
  bwrite(b);
  brelse(b);
  if (len == n) return len;
  write_len += len;
  for (secNo = secNo + 1; write_len < n; secNo++) {
    len = MIN(fs->superblock.bytes_per_sector, n - write_len);
    b = bread(fs->deviceNum, secNo);
    memcpy(b->data, (void *)src + write_len, len);
    bwrite(b);
    brelse(b);
    write_len += len;
  }
  return write_len;
}
