#include <types.h>
#include <macro.h>
#include <list.h>
#include <fat32.h>
#include <fatfs.h>
#include <dirent.h>
#include <string.h>

static struct dirent dirent_poll[MAX_DIRENT];
static struct list   dirent_list;

void dirent_init(void) {
  list_init(&dirent_list);
  for (int i = 0; i < MAX_DIRENT; i++) {
    list_push_back(&dirent_list, &dirent_poll[i].elem);
  }
}

struct dirent *dirent_alloc() {
  if (!list_empty(&dirent_list)) {
    struct dirent *dir = list_entry(list_pop_front(&dirent_list), struct dirent, elem);
    memset(dir, 0, sizeof(*dir));
    return dir;
  } else {
    return NULL;
  }
}

void dirent_free(struct dirent *dir) {
  list_push_back(&dirent_list, &dir->elem);
}
