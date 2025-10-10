/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

#include "vm/vm.h"
#include "vm/uninit.h"

static bool uninit_initialize(struct page *page, void *kva);
static void uninit_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
    .swap_in = uninit_initialize,
    .swap_out = NULL,
    .destroy = uninit_destroy,
    .type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void uninit_new(struct page *page, void *va, vm_initializer *init, enum vm_type type, void *aux,
                bool (*initializer)(struct page *, enum vm_type, void *)) {
  ASSERT(page != NULL);

  *page = (struct page){.operations = &uninit_ops,
                        .va = va,
                        .frame = NULL, /* no frame for now */
                        .uninit = (struct uninit_page){
                            .init = init,
                            .type = type,
                            .aux = aux,
                            .page_initializer = initializer,
                        }};
}

/* Initalize the page on first fault */
// SPT에 "uninit 상태"로 등록해둔 페이지를 실제 물리 프레임에 연결하면서 초기화하는 함수.
//  uninit_initialize 호출 후 anon_initializer 이거나
//  uninit_initialize 호출 후 file_backed_initialize
static bool uninit_initialize(struct page *page, void *kva) {
  struct uninit_page *uninit = &page->uninit;

  /* Fetch first, page_initialize may overwrite the values */
  vm_initializer *init = uninit->init;
  void *aux = uninit->aux;

  /* TODO: You may need to fix this function. */
  return uninit->page_initializer(
             page, uninit->type,
             kva) &&  // uninit->page_initializer (page, uninit->type, kva) : 타입별 초기화기 준비
         (init ? init(page, aux)
               : true);  //(init ? init (page, aux) : true) 데이터 채우기 단계 memset or file_read
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
static void uninit_destroy(struct page *page) {
  // UNINIT 전용 메타데이터
  struct uninit_page *uninit = &page->uninit;

  // UNINIT 상태에서는 프레임이 없어야 정상
  ASSERT(page->frame == NULL);
  if (page->frame != NULL) {
    vm_free_frame(page->frame);  // 일관된 프레임 해제 경로를 통해 정리합니다.
    page->frame = NULL;
  }
  // 지연 로딩용 보조 데이터(aux)가 없으면 정리할 것이 없음
  if (uninit->aux == NULL) return;
  // Lazy load를 위해 파일 포인터/오프셋/읽을 바이트 수 등의 정보를 aux에 담아 두는데
  // 해당 페이지가 한 번도 클레임/폴트되지 않은 채로 정리되는 경우 메모리가 누수될 수 있음
  // 따라서 UNINIT destroy에서 aux를 해제해 리소스 누수를 막음
  free(uninit->aux);
  // TODO: VM_FILE 등 다른 타입을 지원하게 되면 파일 핸들 정리를 추가

  uninit->aux = NULL;               // 포인터를 비워 이중 해제를 방지
  uninit->init = NULL;              // 추가 초기화 콜백도 제거
  uninit->page_initializer = NULL;  // 타입별 초기화기 포인터 초기화
}
