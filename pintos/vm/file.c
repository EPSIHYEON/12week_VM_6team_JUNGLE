/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "include/userprog/syscall.h"
static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

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
  struct aux *aux = page->uninit.aux;
  struct file_page *file_page = &page->file;

  file_page->file = aux->file;
  file_page->ofs = aux->ofs;
  file_page->zero_bytes = aux->zero_bytes;
  file_page->read_bytes = aux->read_bytes;

  return true;
}

/* Swap in the page by read contents from the file. */
//이미 준비된 빈 공간(frame->kva)에 파일 데이터를 채워 넣는 것.
//메모리에 올라왔다가 공간이 부족해서 쫓겨난(swapped-out) 뒤, 다시 필요해졌을 때 호출
static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page = &page->file;
  lock_acquire(&filesys_lock);
  off_t bytes_read = file_read_at(file_page->file, kva, file_page->read_bytes, file_page->ofs);
  //프레임에 파일 채워넣음

  lock_release(&filesys_lock);
  if (bytes_read != file_page->read_bytes) {
    return false;
  }

  memset(kva + bytes_read, 0, PGSIZE - bytes_read);  // memset(목적지 주소, 채울 값, 채울 크기)
  // page->status = IN_FRAME;
  return true;
}

/* Swap out the page by writeback contents to the file. */
// swap_out는 물리페이지에서 매핑을 해제하는 것, spt에는 남아있다
// page->va : 이 페이지가 유저 주소공간에서 매핑될 위치  ex. upage, va
// page->frame->kva : 커널이 실제 데이터를 접근하는 물리 주소 ex. palloc_get_page()
static bool file_backed_swap_out(struct page *page) {
  struct file_page *file_page = &page->file;
  if (page->frame == NULL) return false;  // 물리 메모리 없으면 넘어감

  // lock_acquire(&filesys_lock);

  if (pml4_is_dirty(thread_current()->pml4, page->va)) {  // 수정된 사항이 있으면
    off_t bytes_written = file_write_at(file_page->file, page->frame->kva, file_page->read_bytes,
                                        file_page->ofs);  //다시 쓰고
    if (bytes_written != file_page->read_bytes) {
      // lock_release(&filesys_lock);
      return false;
    }  // 제대로 썼는지 확인
  }

  // lock_release(&filesys_lock);
  pml4_clear_page(thread_current()->pml4, page->va);  //물리 메모리 매핑 끊음

  // page->status = IN_FILE;  // status 변화함

  // vm_free_frame(page->frame);  // 프레임 해제
  // page->frame = NULL;
  page->frame->page = NULL;
  page->frame = NULL;

  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
  struct file_page *file_page = &page->file;
  lock_acquire(&filesys_lock);
  if (page->frame != NULL && pml4_is_dirty(thread_current()->pml4, page->va)) {
    off_t written =
        file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
    ASSERT(written == file_page->read_bytes);
  }  // dirty 하다면 파일 쓰기

  lock_release(&filesys_lock);

  pml4_clear_page(thread_current()->pml4, page->va);  // 매핑 해제

  if (page->frame != NULL) {
    vm_free_frame(page->frame);
    page->frame = NULL;
  }
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset) {}

/* Do the munmap */
void do_munmap(void *addr) {}