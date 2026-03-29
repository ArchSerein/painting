#include <types.h>
#include <param.h>
#include <memlayout.h>
#include <spinlock.h>
#include <sleeplock.h>
#include <list.h>
#include <buf.h>
#include <virt.h>
#include <macro.h>
#include <stdio.h>
#include <string.h>
#include <pm.h>
#include <defs.h>
#include <riscv.h>

static struct disk disk;

void virtio_disk_init(void) {
  
  uint32_t status = 0;

	// 检查设备的魔术值、版本、设备ID和厂商ID，确保找到了virtio磁盘设备。如果条件不满足，会触发panic
	if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 || *R(VIRTIO_MMIO_VERSION) != 2 ||
	    *R(VIRTIO_MMIO_DEVICE_ID) != 2 || *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
		panic("could not find virtio disk");
	}

  // during initialize
  *R(VIRTIO_MMIO_STATUS) = status;
 
  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGES;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64_t features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32_t max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kpmalloc();
  disk.avail = kpmalloc();
  disk.used = kpmalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64_t)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64_t)disk.desc >> 32;
  *R(VIRTIO_MMIO_QUEUE_AVAIL_LOW) = (uint64_t)disk.avail;
  *R(VIRTIO_MMIO_QUEUE_AVAIL_HIGH) = (uint64_t)disk.avail >> 32;
  *R(VIRTIO_MMIO_QUEUE_USED_LOW) = (uint64_t)disk.used;
  *R(VIRTIO_MMIO_QUEUE_USED_HIGH) = (uint64_t)disk.used >> 32;

  // queue is ready.
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VIRTQ_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64_t sector = b->blockno * (BSIZE / 512);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    panic("no free desc");
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = (uint64_t) buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64_t) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VIRTQ_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VIRTQ_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = (uint64_t) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VIRTQ_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // tell the device the first index in our chain of descriptors.
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize();

  // tell the device another avail ring entry is available.
  disk.avail->idx += 1; // not % NUM ...

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    __sync_synchronize();
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);
}

void
virtio_disk_intr()
{
#ifdef CONFIG_DEBUG
  #ifdef CONFIG_DEBUG
  // log("entry virtio_disk_intr\n");
  #endif
#endif

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0) {
      #ifdef CONFIG_DEBUG
      log("disk id: %d, status: %d\n", id, disk.info[id].status);
      #endif
      panic("virtio_disk_intr status");
    }

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    __sync_synchronize();

    disk.used_idx += 1;
  }
}

void virtioTest() {
	#ifdef CONFIG_DEBUG
	log("begin virtio test!\n");
	#endif
	struct buf bufR, bufW;

	// 测试写入0号扇区（块）
	bufW.blockno = 0;
	for (int i = 0; i < BSIZE; i++) {
		bufW.data[i] = '0' + i % 10;
	}
	bufW.data[BSIZE - 1] = 0;
	virtio_disk_rw(&bufW, 1); // write

	// 测试读出0号扇区
	bufR.blockno = 0;
	virtio_disk_rw(&bufR, 0); // read
	ASSERT(strncmp((const char *)bufR.data, (const char *)bufW.data, BSIZE) == 0);

	// 测试写入1号扇区
	bufW.blockno = 1;
	for (int i = 0; i < BSIZE; i++) {
		bufW.data[i] = '2' + i % 6;
	}
	virtio_disk_rw(&bufW, 1); // write

	// 测试读出1号扇区
	bufR.blockno = 1;
	virtio_disk_rw(&bufR, 0); // read
	ASSERT(strncmp((const char *)bufR.data, (const char *)bufW.data, BSIZE) == 0);

	#ifdef CONFIG_DEBUG
	log("virtio driver test passed!\n");
	#endif
}

