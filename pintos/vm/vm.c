/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "kernel/hash.h"
#include "threads/malloc.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
  vm_anon_init();
  vm_file_init();
#ifdef EFILESYS /* For project 4 */
  pagecache_init();
#endif
  register_inspect_intr();
  /* DO NOT MODIFY UPPER LINES. */
  /* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
    default:
      return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
//데이터를 만들겠다는 약속을 설정하는 함수, load_segment한테서 aux 구조체를 받고
// uninit_new를 생성한다, 페이지 폴트 전에 설정을 하는 함수 [unint으로 설정된
//상태, 데이터는 아작 안올라감 ]
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux) {
  ASSERT(VM_TYPE(type) != VM_UNINIT)

  struct supplemental_page_table *spt = &thread_current()->spt;

  /* Check wheter the upage is already occupied or not. */
  // upage라는 주소를 가보았더니 아무것도 없다(채워 넣어도 된다)
  if (spt_find_page(spt, upage) == NULL) {
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */
    struct page *page = malloc(sizeof(struct page));
    if (page == NULL) return false;

    if (type == VM_ANON) {
      uninit_new(page, upage, init, type, aux, anon_initializer);
    } else if (type == VM_FILE) {
      uninit_new(page, upage, init, type, aux, file_backed_initializer);
    }
    page->writable = writable;

    /* TODO: Insert the page into the spt. */
    if (!spt_insert_page(spt, page)) {
      free(page);
      return false;
    }

    return true;
  }
  return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt, void *va UNUSED) {
  struct page page;
  page.va = va;

  struct hash_elem *e = hash_find(&spt->h, &page.h_elem);
  if (e != NULL) {
    struct page *page = hash_entry(e, struct page, h_elem);
    return page;
  }

  return NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page) {
  int succ = false;
  /* TODO: Fill this function. */
  if (hash_insert(&spt->h, &page->h_elem) == NULL) {
    succ = true;
  }
  return succ;
}

void spt_remove_page(struct supplemental_page_table *spt UNUSED, struct page *page) {
  vm_dealloc_page(page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  struct frame *victim = NULL;
  /* TODO: The policy for eviction is up to you. */

  return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim UNUSED = vm_get_victim();
  /* TODO: swap out the victim and return the evicted frame. */

  return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/* 새로운 물리 프레임을 얻을 때 사용 */
static struct frame *vm_get_frame(void) {
  struct frame *frame = NULL;
  /* TODO: Fill this function. */

  /* 물리주소 할당
   * PAL_USER 는 메모리 풀(커널/유저) 중 유저풀
   */
  void *kva = palloc_get_page(PAL_USER);
  if (!kva) {
    PANIC("todo:swap-out");
  }
  frame = malloc(sizeof(struct frame));
  if (!frame) {
    PANIC("todo:?");
  }
  /* 프레임 초기화 */
  frame->kva = kva;
  frame->page = NULL;

  ASSERT(frame != NULL);
  ASSERT(frame->page == NULL);
  return frame;
}

/* Growing the stack. */
/**
 * 페이지 폴트가 스택 영역에서 발생했고, 스택 확장 조건을 만족할 때
 * 해당 폴트 주소를 포함하는 한 개 이상의 익명 페이지를 할당,매핑하여 스택을 아래로 확장
 * addr : 폴트가 발생한 가상 주소
 */
static void vm_stack_growth(void *addr) {
  // 주소 유효성 검사
  if (addr == NULL || !is_user_vaddr(addr)) return;
  // 현재 스레드의 SPT
  struct supplemental_page_table *spt = &thread_current()->spt;
  // SPT의 키 (페이지 시작 주소)
  void *page_addr = pg_round_down(addr);
  // 1MB 하한을 계산. USER_STACK(스택 꼭대기)에서 1MB 내려간 주소가 최저 허용 주소
  uint8_t *stack_limit = (uint8_t *)(USER_STACK - (1 << 20));

  if ((uint8_t *)page_addr < stack_limit) return;  // 허용 범위를 벗어나면 확장하지 않음.

  while ((uint8_t *)page_addr < (uint8_t *)USER_STACK) {
    // 현재 page_addr 위치에 이미 페이지가 등록되어 있으면 중단
    if (spt_find_page(spt, page_addr) != NULL) break;
    // 해당 가상주소에 익명 페이지를 UNINIT 형태로 SPT에 등록. 쓰기 가능=true
    if (!vm_alloc_page_with_initializer(VM_ANON, page_addr, true, NULL, NULL)) break;
    // 방금 등록한 페이지를 즉시 클레임(프레임 할당 + PTE 매핑)
    if (!vm_claim_page(page_addr)) break;
    // 다음 페이지(더 위쪽 주소)로 이동
    page_addr = (uint8_t *)page_addr + PGSIZE;
  }
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page) {
  if (page == NULL)  // 페이지 정보가 없으면 복구할 방법이 없습니다.
    return false;

  if (!page->writable)  // 설계상 쓰기가 허용되지 않은 페이지는 그대로 실패 처리합니다.
    return false;

  // 현재 단계에서는 Copy-on-Write 등의 기능이 없으므로
  // writable 페이지에서 발생한 쓰기 보호 폴트도 복구하지 않습니다.
  return false;
}

/* Return true on success */
/**
 * 페이지 폴트에 대한 전반적인 처리 함수
 * f : 폴트 당시의 CPU 레지스터 상태를 담은 인터럽트 프레임
 * addr : 폴트가 발생한 가상주소
 * user : 폴트가 사용자 모드에서 발생했는지 여부
 * write : 접근이 쓰기였는지 여부
 * not_present : PTE에 주소 매핑 자체가 존재하는지
 * - true : PTE 자체가 없음 -> SPT를 확인하여 해결
 * - false : PTE는 있는데 잘못된 접근임
 * 읽기 전용 페이지에 쓰기 시도, 사용자 모드에서 커널 전용 페이지 접근 등
 */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED,
                         bool write UNUSED, bool not_present UNUSED) {
  // 현재 폴트를 일으킨 스레드
  struct thread *curr = thread_current();
  // 스레드가 보유한 보조 페이지 테이블
  struct supplemental_page_table *spt = &curr->spt;
  // 폴트 주소에 대응하는 페이지 객체를 담을 변수
  struct page *page = NULL;

  // 1. 잘못된 접근 필터링
  // NULL 주소 접근
  if (addr == NULL) return false;
  // 사용자 모드에서 커널 영역 접근
  if (user && is_kernel_vaddr(addr)) return false;
  // 항상 유저 영역만 허용
  if (!is_user_vaddr(addr)) return false;

  // 폴트가 난 주소를 페이지 경계로 내림. SPT의 페이지 키값
  void *fault_page = pg_round_down(addr);

  // 2. PTE는 존재하지만 권한 문제 등으로 폴트가 난 경우
  if (!not_present) {
    if (write) {
      page = spt_find_page(spt, fault_page);
      return vm_handle_wp(page);
    }
    return false;
  }

  // 3. SPT 조회
  // 우선 SPT에서 해당 주소의 페이지를 찾음
  page = spt_find_page(spt, fault_page);
  // 등록된 페이지가 없다면 스택 확장 여부를 검토
  if (page == NULL) {
    // 폴트 당시의 스택 포인터를 결정
    uint8_t *rsp = user ? (uint8_t *)f->rsp : (uint8_t *)curr->tf.rsp;
    // 비교 연산을 위해 폴트 주소를 바이트 포인터로 변환
    uint8_t *addr_u8 = (uint8_t *)addr;
    // 임의로 1MB 크기의 스택 하한선을 설정
    uint8_t *stack_lower_bound = (uint8_t *)(USER_STACK - (1 << 20));

    // grow 가능 범위인지 확인
    if (rsp != NULL && addr_u8 >= stack_lower_bound) {
      // 스택 페이지를 새로 확보하도록 요청
      vm_stack_growth(fault_page);
      // 스택 확장 후 다시 페이지 객체를 확인
      page = spt_find_page(spt, fault_page);
    }
    // 스택 확장까지 실패했다면 더 이상 처리할 방법이 없음
    if (page == NULL) return false;
  }
  // 실제 프레임을 매핑하는 단계가 실패하면 폴트 처리 실패로 반환
  if (!vm_do_claim_page(page)) return false;
  // 모든 절차를 완료했으면 폴트 처리를 성공
  return true;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/* Claim the page that allocate on VA. */
/* 유저가상주소(va)에 대한 페이지를 찾아서 vm_do_claim_page()로 전달 */
bool vm_claim_page(void *va) {
  struct page *page = NULL;
  /* TODO: Fill this function */
  struct supplemental_page_table *spt = &thread_current()->spt;
  page = spt_find_page(spt, va);
  /* 페이지 없는 경우 */
  if (!page) return false;  // 등록된 페이지가 없으면 실패.
  return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
/* 페이지와 프레임을 매핑 */
static bool vm_do_claim_page(struct page *page) {
  struct frame *frame = vm_get_frame();
  if (!frame) {
    return false;
  }

  /* Set links */
  frame->page = page;
  page->frame = frame;

  if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) return false;

  return swap_in(page, frame->kva);  // 페이지 타입별 swap_in 구현이 실제 초기화 작업을 수행.
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt) {
  struct hash *spt_hash = &spt->h;
  hash_init(spt_hash, page_hash_func, compare_hash_adrr, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
                                  struct supplemental_page_table *src) {
  struct hash_iterator i;

  hash_first(&i, &src->h);  //부모 SPT를 순회
  while (hash_next(&i)) {
    struct page *parent_page = hash_entry(hash_cur(&i), struct page, h_elem);
    enum vm_type type = page_get_type(parent_page);
    void *upage = parent_page->va;
    bool writable = parent_page->writable;

    switch (type) {
      case VM_UNINIT:
        /* UNINIT 페이지: 아직 물리 메모리에 로드되지 않은 페이지
         * 부모의 초기화 정보를 그대로 자식에게 물려줌 */
        struct uninit_page *uninit = &parent_page->uninit;
        if (!vm_alloc_page_with_initializer(VM_UNINIT, upage, writable, uninit->init, uninit->aux))
          return false;
        break;

      case VM_ANON:
      case VM_FILE:
        /* ANON 또는 FILE 페이지: 이미 물리 메모리에 내용이 로드된 페이지
           독자적인 물리 공간을 가져야 함
        */
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, NULL, NULL)) {
          return false;
        }  // SPT 에 빈공간 할당

        // 2. 빈공간을 자식의 페이지를 SPT에서 다시 찾음
        struct page *child_page = spt_find_page(dst, upage);
        if (child_page == NULL) {
          supplemental_page_table_kill(dst);
          return false;
        }
        // 3. 자식 페이지에 물리 프레임을 할당, 페이지 테이블에 매핑
        if (!vm_claim_page(child_page->va)) {
          supplemental_page_table_kill(dst);
          return false;
        }
        memcpy(child_page->frame->kva, parent_page->frame->kva,
               PGSIZE);  //빈공간에 부모 내용 자식에 쓰기 //커널 가상 주소(Kernel Virtual Address)
        break;
    }
  }

  // 모든 페이지 복사가 성공적으로 끝나면 true를 반환
  return true;
}

//보조 페이지 테이블(Supplemental Page Table, SPT)의 자원을 해제
void supplemental_page_table_kill(struct supplemental_page_table *spt) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */

  struct hash_iterator i;

  for (hash_first(&i, &spt->h); hash_cur(&i);) {
    struct page *current_page = hash_entry(hash_cur(&i), struct page, h_elem);

    //반복자를 안전하게 다음으로 미리 이동
    hash_next(&i);

    //저장해둔 current_page의 자원을 정리
    destroy(current_page);
    // hash에 current_page 제거
    hash_delete(&spt->h, &current_page->h_elem);
    // current_page 제거
    free(current_page);
  }
}

uint64_t page_hash_func(const struct hash_elem *elem, void *aux UNUSED) {
  const struct page *p = hash_entry(elem, struct page, h_elem);

  return hash_bytes(&p->va, sizeof(p->va));
}

bool compare_hash_adrr(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
  struct page *p_a = hash_entry(a, struct page, h_elem);
  struct page *p_b = hash_entry(b, struct page, h_elem);

  return p_a->va > p_b->va;
}
