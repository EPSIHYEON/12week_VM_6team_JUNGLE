/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <bitmap.h>  // 스왑 슬롯 사용 여부를 추적하는 비트맵 유틸리티.

#include "debug.h"
#include "devices/disk.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/vm.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;      // 스왑 디스크 장치를 가리키는 포인터.
static struct bitmap *swap_bitmap;  // 스왑 슬롯의 사용 여부를 표시하는 비트맵.
static struct lock swap_lock;       // 스왑 슬롯 할당을 동기화하는 락.
static size_t swap_slot_cnt;        // 사용 가능한 스왑 슬롯 개수.
static size_t sectors_per_page;     // 한 페이지를 저장하는 데 필요한 섹터 수.
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
/**
 * 익명 페이지 서브시스템을 초기화하는 함수의 시작
 * 시스템 부팅 시 VM 초기화 루틴(vm_init)에서 한 번 호출됨
 */
void vm_anon_init(void) {
  /* TODO: Set up the swap_disk. */
  swap_disk = disk_get(1, 1);  // 채널 1, 장치 1에 있는 스왑 디스크를 얻음.
  // 페이지당 필요한 섹터 수를 계산.
  sectors_per_page = PGSIZE / DISK_SECTOR_SIZE;
  ASSERT(sectors_per_page > 0);  // 최소 한 섹터는 필요하다는 점을 확인.

  if (swap_disk != NULL) {
    // 스왑 디스크 전체 섹터 수.
    disk_sector_t total_sectors = disk_size(swap_disk);
    // 스왑 가능한 페이지 슬롯 개수 계산.
    swap_slot_cnt = total_sectors / sectors_per_page;
    // 슬롯 사용 현황을 추적할 비트맵 생성.
    swap_bitmap = bitmap_create(swap_slot_cnt);
    if (swap_bitmap == NULL)
      PANIC("swap bitmap creation failed");  // 비트맵 생성 실패 시 즉시 중단.
    lock_init(&swap_lock);                   // 동시 스왑을 위한 락 초기화.
  } else {
    swap_slot_cnt = 0;   // 스왑 디스크가 없으면 슬롯이 없음.
    swap_bitmap = NULL;  // 스왑을 쓰지 않을 경우 비트맵을 비활성화.
  }
}

/* Initialize the file mapping */
/**
 * UNINIT 상태의 페이지가 처음 접근될 때(페이지 폴트) 익명(anonymous) 페이지로
전환하기 위한 타입 초기화 함수
 * page: 초기화 대상 페이지 객체
 * type: 요청된 VM타입(마커 비트 포함 가능)
 * kva: 이 페이지가 매핑될 물리 프레임의 커널 가상주소.
 */
bool anon_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &anon_ops;  // 익명 페이지용 연산 테이블을 연결.

  ASSERT(VM_TYPE(type) == VM_ANON);  // 호출자가 익명 타입을 요청했는지 확인.
  // 페이지별 익명 메타데이터에 접근.
  struct anon_page *anon_page = &page->anon;
  // 아직 스왑에 기록되지 않았음을 표시.
  anon_page->swap_slot = ANON_SWAP_SLOT_INVALID;
  (void)kva;    // 프레임을 클레임하기 전까지 KVA는 사용되지 않음.
  return true;  // 초기화가 성공했음을 알림.
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page) { struct anon_page *anon_page = &page->anon; }

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) {
  // 익명 페이지 메타데이터(스왑 슬롯 정보)
  struct anon_page *anon = &page->anon;
  struct thread *t = thread_current();

  // 메모리에 적재된 상태였다면 프레임을 해제
  if (page->frame != NULL) {
    // pte 언매핑
    if (pml4_get_page(t->pml4, page->va) != NULL) {
      pml4_clear_page(t->pml4, page->va);
    }
    vm_free_frame(page->frame);
    page->frame = NULL;
  }
  // 스왑 슬롯을 사용했다면 반환
  if (swap_bitmap != NULL && anon->swap_slot != ANON_SWAP_SLOT_INVALID) {
    lock_acquire(&swap_lock);
    // 슬롯을 비어 있음으로 표시 (false)
    bitmap_set(swap_bitmap, anon->swap_slot, false);
    // 슬롯 번호를 초기화해 재사용을 방지
    anon->swap_slot = ANON_SWAP_SLOT_INVALID;
    lock_release(&swap_lock);
  }
}
