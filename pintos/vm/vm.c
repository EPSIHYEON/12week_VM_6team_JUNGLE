/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "kernel/hash.h"
#include "threads/malloc.h"
#include "vm/inspect.h"
#include <string.h>
#ifdef USERPROG
#include "userprog/syscall.h"
#endif

#define STACK_MAX_BYTES (1 << 20) /* 1 MB stack limit. 스택 하한: USER_STACK - 1MB */
#define STACK_HEURISTIC 32 /* RSP 근처 허용 거리(바이트). 너무 작으면 pusha류 테스트 실패 */

/* 페이지 교체 알고리즘을 위한 전역변수 추가 */
struct list frame_list;
struct lock frame_lock;
static struct list_elem *clock_pointer;

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
  list_init(&frame_list);
  lock_init(&frame_lock);
  clock_pointer = NULL;
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
/* 스택 성장 가능 여부 판단: 유저공간/상하한/near-RSP(32B)를 모두 검사 */
static bool stack_growth_allowed(uint8_t *fault_addr, uint8_t *rsp_snapshot);
/* 스택 성장 수행: fault 페이지 한 개를 UNINIT(ANON|MARKER)으로 SPT에 등록 */
static bool vm_stack_growth(void *addr);
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

  /* 이미 동일 VA가 SPT에 있으면 실패. 비어있으면 등록 가능 */
  if (spt_find_page(spt, upage) == NULL) {
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */
    struct page *page = malloc(sizeof(struct page));
    if (page == NULL) return false;

    // VM_ANON/VM_FILE 같은 원시 타입에 VM_MARKER_0 등 플래그를 제거하고 순수한 타입만 추출
    enum vm_type primitive_type = VM_TYPE(type);
    // 로그
    // printf("[VM_ALLOC] VA: %p, Type: %s\n", upage, (primitive_type == VM_ANON ? "ANON" :
    // "FILE"));

    if (primitive_type == VM_ANON) {
      uninit_new(page, upage, init, type, aux, anon_initializer);
    } else if (primitive_type == VM_FILE) {
      uninit_new(page, upage, init, type, aux, file_backed_initializer);
    } else {
      // 지원하지 않는 타입에 대해 free
      free(page);
      return false;
    }
    page->writable = writable; /* 스택 페이지는 true 이어야 함 */

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

/* 페이지 교체 알고리즘 : Clock 알고리즘
 * LRU(Least Recently Used) 흉내내는 알고리즘으로 원형 리스트를 돌면서 최근에 접근한 적 없는
 * 페이지를 선택 */
static struct frame *vm_get_victim(void) {
  /* 리스트가 비어있는 경우 커널패닉 */
  ASSERT(!list_empty(&frame_list));

  /* 전역변수 프레임 리스트에 대한 락 획득 */
  lock_acquire(&frame_lock);
  /* Clock 초기 위치 세팅 */
  // printf("\n[CLOCK] === start vm_get_victim ===\n");
  // printf("[CLOCK] frame_list size=%zu\n", list_size(&frame_list));

  if (clock_pointer == NULL || clock_pointer == list_end(&frame_list)) {
    clock_pointer = list_begin(&frame_list);
    // printf("[CLOCK] reset clock_pointer -> list_begin\n");
  }
  while (true) {
    struct frame *victim = list_entry(clock_pointer, struct frame, elem);
    struct page *page = victim->page;
    struct thread *curr = thread_current();

    // 유저스택영역은 제외
    // if (page_get_type(page) & VM_MARKER_0) continue;  // or skip adding to frame_list
    // if (page->va >= USER_STACK - PGSIZE && page->va < USER_STACK) {
    //   clock_pointer = list_next(clock_pointer);
    //   if (clock_pointer == list_end(&frame_list)) clock_pointer = list_begin(&frame_list);
    //   continue;
    // }

    /* 접근여부 검사 */
    if (page != NULL) {
      if (pml4_is_accessed(curr->pml4, page->va)) {
        /* 최근 접근한 페이지는 한번 봐줌 */
        pml4_set_accessed(curr->pml4, page->va, false);
        // printf("[CLOCK] reset accessed bit for va=%p\n", page->va);
      } else {
        /* 그 외 페이지 선택 */
        clock_pointer = list_next(clock_pointer);
        lock_release(&frame_lock);
        // printf("[CLOCK] choose victim: frame=%p, page=%p, va=%p\n", victim, page,
        //        page ? page->va : NULL);
        return victim;
      }
    }
    /* 다음 프레임으로 넘어감(끝에 도달 시 다시 처음부터)*/
    clock_pointer = list_next(clock_pointer);
    if (clock_pointer == list_end(&frame_list)) {
      clock_pointer = list_begin(&frame_list);
    }
  }
  lock_release(&frame_lock);
  return NULL;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  /* 1. 희생할 프레임 선택 */
  struct frame *victim = vm_get_victim();
  ASSERT(victim != NULL);
  ASSERT(victim->page != NULL);
  struct page *page = victim->page;
  /* 2. swap-out(anon -> swap 영역, file -> write-back) */
  swap_out(page);
  return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/* 새로운 물리 프레임을 얻을 때 사용 */
static struct frame *vm_get_frame(void) {
  struct frame *frame;
  void *kva = palloc_get_page(PAL_USER);
  /* 할당 실패 시 기존 프레임 재활용 */
  if (!kva) {
    frame = vm_evict_frame();
    /* 프레임 초기화 */
    frame->page = NULL;
    return frame;
  }

  /* 새로 할당되는 경우 */
  frame = malloc(sizeof(struct frame));
  /* 프레임 초기화 */
  frame->kva = kva;
  frame->page = NULL;
  /* 프레임 리스트에 추가 */
  lock_acquire(&frame_lock);
  list_push_back(&frame_list, &frame->elem);
  lock_release(&frame_lock);

  ASSERT(frame != NULL);
  ASSERT(frame->page == NULL);
  return frame;
}

void vm_free_frame(struct frame *frame) {
  if (frame == NULL) return;
  // TODO: 프레임 테이블 도입 시 락, 전역 자료구조, 핀 처리 구현

  // 페이지와의 양방향 연결을 끊기
  if (frame->page != NULL) {
    if (frame->page->frame == frame) frame->page->frame = NULL;
    frame->page = NULL;
  }

  //프레임 리스트에서 제거
  lock_acquire(&frame_lock);
  list_remove(&frame->elem);
  lock_release(&frame_lock);

  // 실제 물리 페이지 반환
  // printf("[FREE_PAGE] kva=%p pg_ofs=%#lx\n", frame->kva, pg_ofs(frame->kva));
  palloc_free_page(frame->kva);
  // 프레임 구조체 해제
  free(frame);
}

/* Growing the stack. */
/* 스택 성장을 허용할지 전체 조건을 검사한다. */
static bool stack_growth_allowed(uint8_t *fault_addr, uint8_t *rsp_snapshot) {
  /* 널 주소는 성장을 허용하지 않는다. */
  if (fault_addr == NULL) return false;
  /* 유저 영역 주소가 아니면 성장을 허용하지 않는다. */
  if (!is_user_vaddr(fault_addr)) return false;
  /* USER_STACK(스택 꼭대기) 이상이면 성장을 허용하지 않는다. */
  if (fault_addr >= (uint8_t *)USER_STACK) return false;
  /* USER_STACK - STACK_MAX_BYTES(1MB) 아래면 성장을 허용하지 않는다. */
  if (fault_addr < (uint8_t *)USER_STACK - STACK_MAX_BYTES) return false;
  /* 사용자 RSP 스냅샷이 없으면 near-RSP 판단이 불가능하므로 거부한다. */
  if (rsp_snapshot == NULL) return false;

  /* RSP 아래쪽으로 살짝 벗어난 접근만 허용: 너무 멀면 잘못된 접근으로 간주 */
  if (fault_addr < rsp_snapshot) {
    /* RSP와 fault 사이 거리(바이트)를 구한다. */
    size_t diff = (size_t)(rsp_snapshot - fault_addr);
    /* 허용 오차(STACK_HEURISTIC)를 넘으면 성장을 허용하지 않는다. */
    if (diff > STACK_HEURISTIC) return false;
  }

  /* 모든 조건을 통과했으므로 성장을 허용한다. */
  return true;
}

/* fault 주소가 속한 페이지 한 장을 UNINIT(ANON)으로 등록 */
/* fault 주소가 속한 페이지 한 장을 스택으로 등록(UNINIT)한다. */
static bool vm_stack_growth(void *addr) {
  /* 널이거나 유저 영역이 아니면 스택 성장 대상이 아니다. */
  if (addr == NULL || !is_user_vaddr(addr)) return false;

  /* 현재 스레드의 SPT를 얻는다. */
  struct supplemental_page_table *spt = &thread_current()->spt;
  /* fault 주소를 페이지 경계로 내린다. */
  void *page_addr = pg_round_down(addr);

  /* 스택 하한(= USER_STACK - 1MB) 아래면 스택 성장을 허용하지 않는다. */
  if ((uint8_t *)page_addr < (uint8_t *)USER_STACK - STACK_MAX_BYTES) return false;
  /* USER_STACK(스택 꼭대기) 이상의 주소는 스택이 아니다. */
  if ((uint8_t *)page_addr >= (uint8_t *)USER_STACK) return false;

  /* 이미 같은 VA가 SPT에 있으면 별도 등록 없이 성공 처리한다. */
  if (spt_find_page(spt, page_addr) != NULL) return true;

  /* 해당 페이지를 익명 스택 페이지로 UNINIT 등록(writable=true). */
  return vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, page_addr, true, NULL, NULL);
}

/* 외부에서 성장 조건 판단 + 실제 등록을 한 번에 수행 */
bool vm_try_stack_growth(void *addr, void *rsp_snapshot) {
  return stack_growth_allowed((uint8_t *)addr, (uint8_t *)rsp_snapshot) && vm_stack_growth(addr);
}

/* Handle the fault on write_protected page */
/* write-protect 폴트 처리: 현재 단계에선 CoW 미구현으로 실패 처리 */
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
/*
 * 페이지 폴트 처리 흐름:
 * 1) 주소/권한 필터링 (유저공간/상한/하한)
 * 2) 권한 폴트면(write) WP 처리 시도 (현 단계는 실패)
 * 3) not-present면 SPT 조회 → 없으면 스택 성장 조건 검사 + 등록
 * 4) 프레임 보유 여부에 따라 pml4_set_page 또는 vm_do_claim_page 수행
 */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write,
                         bool not_present) {
  struct thread *curr = thread_current();
  struct supplemental_page_table *spt = &curr->spt;

  if (addr == NULL) return false;
  if (user && is_kernel_vaddr(addr)) return false;
  if (!is_user_vaddr(addr)) return false;
  if ((uint8_t *)addr >= (uint8_t *)USER_STACK) return false;

  void *fault_page = pg_round_down(addr);
  struct page *page = NULL;

  /* 권한 문제: r/o에 write 등 */
  if (!not_present) {
    if (write) {
      page = spt_find_page(spt, fault_page);
      return vm_handle_wp(page);
    }
    return false;
  }

  /* SPT에서 fault 페이지를 찾는다. */
  page = spt_find_page(spt, fault_page);
  if (page == NULL) {
    /* user 폴트면 f->rsp, syscalls 등 커널 컨텍스트 폴트면 스냅샷 사용 */
    uint8_t *rsp_snapshot = user ? (uint8_t *)f->rsp : curr->user_rsp;
    /* 조건을 만족하면 스택을 한 페이지 확장(UNINIT 등록)한다. */
    if (!vm_try_stack_growth(addr, rsp_snapshot)) return false;
    /* 확장 후 다시 SPT에서 페이지를 조회한다. */
    page = spt_find_page(spt, fault_page);
    /* 그래도 없다면 실패 처리한다. */
    if (page == NULL) return false;
  }

  /* 페이지가 이미 프레임을 보유: PTE 매핑만 하면 된다. */
  if (page->frame != NULL) {
    if (pml4_get_page(curr->pml4, page->va) != NULL) return true;
    return pml4_set_page(curr->pml4, page->va, page->frame->kva, page->writable);
  }

  /* 프레임이 없다면 프레임을 할당하고 swap_in으로 내용을 채운다. */
  return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/* Claim the page that allocate on VA. */
/* 유저가상주소(va)에 대한 페이지를 찾아서 vm_do_claim_page()로 전달 */
/* 외부에서 VA를 주면 SPT에서 찾아 프레임을 연결/매핑 */
bool vm_claim_page(void *va) {
  struct page *page = NULL;
  /* TODO: Fill this function */
  struct supplemental_page_table *spt = &thread_current()->spt;
  page = spt_find_page(spt, va);
  /* 페이지 없는 경우 */
  if (!page) return false;   // 등록된 페이지가 없으면 실패.
  if (page->frame != NULL) { /* 프레임 이미 존재: 재매핑만 하면 됨 */
    if (pml4_get_page(thread_current()->pml4, page->va) != NULL) return true;
    return pml4_set_page(thread_current()->pml4, page->va, page->frame->kva, page->writable);
  }
  return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
/* 페이지와 프레임을 매핑 */
static bool vm_do_claim_page(struct page *page) {
  struct frame *frame = vm_get_frame();
  if (!frame) {
    return false;
  }
  /* page <-> frame 연결 */
  frame->page = page;
  page->frame = frame;

  /* swap_in()을 먼저 호출해서 실제 데이터를 frame->kva에 복구 */
  if (!swap_in(page, frame->kva)) return false;

  /* 물리 페이지 테이블(PML4)에 맵핑 */
  if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) return false;
  return true;
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
    enum vm_type type = parent_page->operations->type;
    enum vm_type real_type = page_get_type(parent_page);
    // printf("[!!!] But the value for switch is type = %d\n", real_type);
    // printf("[!!!] But the value for switch is type = %d\n", type);
    void *upage = parent_page->va;
    bool writable = parent_page->writable;

    switch (type) {
      case VM_UNINIT: {
        /* UNINIT 페이지: 아직 물리 메모리에 로드되지 않은 페이지
         * 부모의 초기화 정보를 그대로 자식에게 물려줌 */
        struct uninit_page *uninit = &parent_page->uninit;
        // 부모 aux 복사
        struct aux *parent_aux = uninit->aux;
        struct aux *child_aux = NULL;
        if (parent_aux != NULL) {
          child_aux = malloc(sizeof(struct aux));
          if (child_aux == NULL) return false;
          *child_aux = *parent_aux;
          child_aux->mapping = NULL;
          if (VM_TYPE(uninit->type) == VM_FILE && parent_aux->file != NULL) {
#ifdef USERPROG
            lock_acquire(&filesys_lock);
#endif
            child_aux->file = file_reopen(parent_aux->file);
#ifdef USERPROG
            lock_release(&filesys_lock);
#endif
            if (child_aux->file == NULL) {
              free(child_aux);
              return false;
            }
          }
        }
        if (!vm_alloc_page_with_initializer(real_type, upage, writable, uninit->init, child_aux)) {
          if (child_aux != NULL) free(child_aux);
          return false;
        }
        break;
      }

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

void supplemental_page_table_kill(struct supplemental_page_table *spt) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
  if (spt == NULL) return;
  if (hash_empty(&spt->h)) return;  // 비어있으면 바로 종료
  struct list to_free;
  list_init(&to_free);
  struct hash_iterator i;
  hash_first(&i, &spt->h);
  while (hash_next(&i)) {
    struct page *current_page = hash_entry(hash_cur(&i), struct page, h_elem);
    list_push_back(&to_free, &current_page->aux_elem);
  }
  while (!list_empty(&to_free)) {
    struct list_elem *e = list_pop_front(&to_free);
    struct page *page = list_entry(e, struct page, aux_elem);
    // printf("[DEBUG] kill: va=%p, frame=%p, type=%d\n", page->va, page->frame,
    // page_get_type(page));
    destroy(page);
    hash_delete(&spt->h, &page->h_elem);
    free(page);
  }
}

// void supplemental_page_table_kill(struct supplemental_page_table *spt) {
//   /* TODO: Destroy all the supplemental_page_table hold by thread and
//    * TODO: writeback all the modified contents to the storage. */

//   struct hash_iterator i;

//   for (hash_first(&i, &spt->h); hash_cur(&i);) {
//     struct page *current_page = hash_entry(hash_cur(&i), struct page, h_elem);

//     //반복자를 안전하게 다음으로 미리 이동
//     hash_next(&i);

//     //저장해둔 current_page의 자원을 정리
//     destroy(current_page);
//     // hash에 current_page 제거
//     hash_delete(&spt->h, &current_page->h_elem);
//     // current_page 제거
//     free(current_page);
//   }
// }
uint64_t page_hash_func(const struct hash_elem *elem, void *aux UNUSED) {
  const struct page *p = hash_entry(elem, struct page, h_elem);

  return hash_bytes(&p->va, sizeof(p->va));
}

bool compare_hash_adrr(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
  struct page *p_a = hash_entry(a, struct page, h_elem);
  struct page *p_b = hash_entry(b, struct page, h_elem);

  return p_a->va < p_b->va;
}
