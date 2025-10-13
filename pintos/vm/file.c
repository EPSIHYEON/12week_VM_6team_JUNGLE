/* file.c: Implementation of memory backed file object (mmaped object). */

#include <round.h>
#include <stdint.h>
#include <string.h>

#include "threads/malloc.h"
#include "userprog/syscall.h"
#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);
static bool mmap_lazy_load(struct page *page, void *aux_);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &file_ops;

  struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;

  // if (pml4_get_page(thread_current()->pml4, page->va) != NULL) {
  //   pml4_clear_page(thread_current()->pml4, page->va);
  // }

  // if (page->frame != NULL && pml4_is_dirty(thread_current()->pml4, page->va)) {
  //   file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
  // }

  // if (page->frame != NULL) vm_free_frame(page->frame);
}

/* Do the mmap */
/**
 * 파일을 addr, addr + length 구간에 지연 로딩 등록하고 실패 시 롤백. 실제 mmap을 수행
 * 인자의 file은 reopen 된 복제 파일 핸들
 */
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset) {
  // 1. 파일 길이와 페이지 수 기본 검증
  if (length == 0) return NULL;
  size_t page_cnt = DIV_ROUND_UP(length, PGSIZE);
  if (page_cnt == 0) return NULL;

  struct thread *curr = thread_current();
  // 2. mmap 관리 객체 mmap_mapping 초기화
  // 나중에 munmap이나 프로세스 종료 시 이 객체를 통해 어떤 페이지들이 이 매핑에 속하는지를 추적
  struct mmap_mapping *mapping = malloc(sizeof(struct mmap_mapping));
  if (mapping == NULL) return NULL;
  mapping->start = addr;
  mapping->length = length;
  mapping->page_cnt = page_cnt;
  mapping->file = file;
  mapping->offset = offset;
  mapping->writable = writable != 0;
  mapping->owner = curr;

  // 이번 매핑에 대해 spt에 성공적으로 등록한 페이지 수 카운터
  size_t inserted = 0;
  off_t file_len;
  // 3. 파일 전체 길이 구하기
  lock_acquire(&filesys_lock);
  file_len = file_length(file);
  lock_release(&filesys_lock);

  // 4. 페이지 별로 lazy 등록
  for (size_t i = 0; i < page_cnt; i++) {
    // addr부터 i번째 페이지의 시작 va
    void *upage = (uint8_t *)addr + i * PGSIZE;
    // 이미 매핑을 앞에서 처리한 이후 남은 바이트
    size_t remaining = length > i * PGSIZE ? length - i * PGSIZE : 0;
    // 남은 바이트 중 이번 페이지가 실제로 처리해야하는 양. 최대 한 페이지
    size_t want = remaining < PGSIZE ? remaining : PGSIZE;
    // mmap 시작 파일 오프셋. 이번 페이지가 어디서 시작하는지
    off_t page_ofs = offset + (off_t)i * PGSIZE;
    size_t file_can = 0;
    // 파일 입장에서 이 페이지가 실제로 채울 수 있는 최대 읽기량 계산
    if (file_len > page_ofs) {
      off_t remain = file_len - page_ofs;
      file_can = remain < (off_t)PGSIZE ? (size_t)remain : PGSIZE;
    }
    // 사용자의 읽기 요청과 파일이 줄 수 있는 양 중 더 작은 값만 실제로 읽기
    size_t read_bytes = want < file_can ? want : file_can;
    // 나머지 공간은 제로 패딩
    size_t zero_bytes = PGSIZE - read_bytes;
    // 이 페이지가 fault 날 때 무엇을 읽고 어떻게 채워야하는지 정보
    struct aux *aux = malloc(sizeof(struct aux));
    if (aux == NULL) {
      goto fail;
    }
    aux->file = file;
    aux->ofs = page_ofs;
    aux->read_bytes = read_bytes;
    aux->zero_bytes = zero_bytes;
    aux->mapping = mapping;
    // SPT에 지연 로딩 페이지로 등록. 나중에 이 페이지가 필요한 순간에 mmap_lazy_load 호출됨
    if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable != 0, mmap_lazy_load, aux)) {
      free(aux);
      goto fail;
    }
    // 성공적으로 등록한 페이지 수 증가
    inserted++;
  }
  // 5. 매핑 등록이 모두 성공
  // 현재 스레드가 소유한 모든 mmap 매핑리스트에 방금 생성한 매핑 객체를 연결
  list_push_back(&curr->mmap_list, &mapping->elem);
  // 매핑 성공한 시작 주소를 반환
  return addr;

fail:
  // 6. 모든 매핑에 실패했을 경우
  // 지금까지 등록한 페이지를 모두 spt에서 제거 후 메모리 해제
  for (size_t j = 0; j < inserted; j++) {
    void *upage = (uint8_t *)addr + j * PGSIZE;
    struct page *page = spt_find_page(&curr->spt, upage);
    if (page != NULL) spt_remove_page(&curr->spt, page);
  }
  free(mapping);
  return NULL;
}

/**
 * 지연로딩으로 디스크에서 실제 데이터를 읽어와 물리 메모리 프레임에 채워넣음
 * page : 지금 fault가 난 실제로 로딩될 페이지 객체
 * aux_ : vm_alloc_page_with_initializer()에서 넘겨준 추가 정보 구조체
 */
static bool mmap_lazy_load(struct page *page, void *aux_) {
  // aux 캐스팅 복원
  struct aux *aux = (struct aux *)aux_;
  if (aux == NULL) return false;

  // 1. file page 초기화 파일 관련 데이터를 복사
  // aux로 전달된 정보들을 실제 페이지 객체가 자기 파일 정보를 직접 들고있게 함
  struct file_page *file_page = &page->file;
  file_page->file = aux->file;
  file_page->ofs = aux->ofs;
  file_page->read_bytes = aux->read_bytes;
  file_page->zero_bytes = aux->zero_bytes;
  file_page->mapping = aux->mapping;
  // 초기화한 파일 정보를 깔끔히 사용하기 위해 임시저장
  struct file *file = file_page->file;
  off_t ofs = file_page->ofs;
  size_t page_read_bytes = file_page->read_bytes;
  size_t page_zero_bytes = file_page->zero_bytes;
  // 올바른 파일 포인터 검증
  if (file == NULL) {
    free(aux);
    return false;
  }

  // 2. 프레임 존재 여부 확인
  // page->frame null : 아직 프레임이 할당되지 않았음
  // page->frame->kva null : 프레임은 있지만 커널에서 접근할 주소가 없음
  if (page->frame == NULL || page->frame->kva == NULL) {
    free(aux);
    file_page->file = NULL;
    return false;
  }
  // 프레임을 커널이 접근할 수 있는 포인터
  uint8_t *kva = page->frame->kva;

  // 3. 파일에서 필요한 만큼 채우기
  // page_read_bytes 이 페이지에서 파일로부터 실제로 읽어와야 할 바이트 수
  // 대개 PGSIZE 또는 마지막 페이지면 파일 끝까지의 나머지 바이트
  if (page_read_bytes > 0) {
    lock_acquire(&filesys_lock);
    // file의 ofs위치부터 page_read_bytes만큼 바로 kva로 읽어옴
    off_t read = file_read_at(file, kva, page_read_bytes, ofs);
    lock_release(&filesys_lock);
    // 읽어온 값이 요청한 바이트 수와 다르면 실패
    if (read != (off_t)page_read_bytes) {
      free(aux);
      file_page->file = NULL;
      file_page->mapping = NULL;
      return false;
    }
  }
  // 남는 공간은 0으로 패딩
  if (page_zero_bytes > 0) memset(kva + page_read_bytes, 0, page_zero_bytes);
  // 로딩이 끝났으니 해제 후 성공 반환
  free(aux);
  return true;
}

/* Do the munmap */
void do_munmap(void *addr) {}
