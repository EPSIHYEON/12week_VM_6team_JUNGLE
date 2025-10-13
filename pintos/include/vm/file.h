#ifndef VM_FILE_H
#define VM_FILE_H
#include <list.h>

#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;
struct mmap_mapping;
struct thread;

struct file_page {
  struct file *file;
  off_t ofs;
  size_t read_bytes;
  size_t zero_bytes;
  struct mmap_mapping *mapping;
};

// 페이지 단위가 아닌 매핑 단위의 정보를 관리
struct mmap_mapping {
  void *start;        // 매핑 시작 va
  size_t length;      // 사용자가 요청한 바이트 길이
  size_t page_cnt;    // 실제 등록된 페이지 수
  struct file *file;  // 매핑 파일의 핸들
  off_t offset;       // 매핑 파일 시작 오프겟
  bool writable;      // 매핑 전체의 쓰기 가능 여부
  struct thread *owner;  // 이 매핑을 소유한 스레드. 더티 판정 pml4 조회, munmap/exit 소유자
  struct list_elem elem;  // thread->mmap_list에 연결하기 위한 리스트 엘리먼트
};

void vm_file_init(void);
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap(void *va);
#endif
