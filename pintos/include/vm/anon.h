#ifndef VM_ANON_H
#define VM_ANON_H
struct page;
enum vm_type;

/* Metadata for anonymous pages. */
struct anon_page {
  size_t swap_slot;  // 이 페이지에 예약된 스왑 슬롯 인덱스(SIZE_MAX는 미배정).
};

#define ANON_SWAP_SLOT_INVALID SIZE_MAX  // 스왑에 기록된 적이 없음을 나타내는 센티널 값.

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif
